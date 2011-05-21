/*
 *	Fusion Kernel Module
 *
 *	(c) Copyright 2002-2003  Convergence GmbH
 *
 *      Written by Denis Oliver Kropp <dok@directfb.org>
 *
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

//#define FUSION_ENABLE_DEBUG

#ifdef HAVE_LINUX_CONFIG_H
#include <linux/config.h>
#endif
#include <linux/version.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
#include <linux/smp_lock.h>
#endif
#include <linux/sched.h>
#include <asm/uaccess.h>

#include <linux/fusion.h>

#include "call.h"
#include "fifo.h"
#include "list.h"
#include "fusiondev.h"
#include "fusionee.h"
#include "property.h"
#include "reactor.h"
#include "ref.h"
#include "skirmish.h"
#include "shmpool.h"

typedef struct {
	FusionLink 		 link;

	int 				 msg_id;

	MessageCallbackFunc	 func;
	void 			*ctx;
	int 				 param;
} MessageCallback;

#define FUSION_MAX_PACKET_SIZE	16384

typedef struct {
	FusionLink 		 link;

	int				 magic;

	char 			 buf[FUSION_MAX_PACKET_SIZE];
	size_t			 size;
	bool				 flush;

	FusionFifo		 callbacks;
} Packet;

/******************************************************************************/

static Packet *
Packet_New( void )
{
	Packet *packet;

	FUSION_DEBUG( "%s()\n", __FUNCTION__ );

	packet = kmalloc( sizeof(Packet), GFP_ATOMIC );
	if (!packet)
		return NULL;

	packet->link.magic = 0;
	packet->link.prev  = NULL;
	packet->link.next  = NULL;

	packet->size 	   = 0;
	packet->flush 	   = false;

	fusion_fifo_reset( &packet->callbacks );

	D_MAGIC_SET( packet, Packet );

	return packet;
}

static void
Packet_Free( Packet *packet )
{
	MessageCallback *callback;

	FUSION_DEBUG( "%s( %p )\n", __FUNCTION__, packet );

	D_MAGIC_ASSERT( packet, Packet );

	D_ASSERT( packet->link.prev == NULL );
	D_ASSERT( packet->link.next == NULL );

	D_MAGIC_CLEAR( packet );

	while ((callback = (MessageCallback *) fusion_fifo_get(&packet->callbacks)) != NULL) {
		D_MAGIC_ASSERT( packet, Packet );

		kfree( callback );
	}

	kfree( packet );
}

static int
Packet_Write( Packet     *packet,
		    int         type,
		    int     	 msg_id,
		    int     	 channel,
		    const void *msg_data,
		    int     	 msg_size,
		    const void *extra_data,
		    int     	 extra_size,
		    bool	  	 from_user )
{
	size_t             total   = sizeof(FusionReadMessage) + msg_size + extra_size;
	size_t             aligned = (total + 3) & ~3;
	FusionReadMessage *header  = (FusionReadMessage *)( packet->buf + packet->size );

	FUSION_DEBUG( "%s( %p, msg_id %d, channel %d, size %d, extra %d, total %zu )\n",
			    __FUNCTION__, packet, msg_id, channel, msg_size, extra_size, total );

	D_MAGIC_ASSERT( packet, Packet );

	FUSION_ASSERT( packet->size + aligned <= FUSION_MAX_PACKET_SIZE );

	header->msg_type    = type;
	header->msg_id      = msg_id;
	header->msg_channel = channel;
	header->msg_size    = msg_size + extra_size;

	if (from_user) {
		if (copy_from_user( header + 1, msg_data, msg_size ))
			return -EFAULT;
	}
	else
		memcpy( header + 1, msg_data, msg_size );

	if (extra_data && extra_size) {
		if (copy_from_user( (char*)(header + 1) + msg_size, extra_data, extra_size ))
			return -EFAULT;
	}

	while (total < aligned)
		packet->buf[packet->size + total++] = 0;

	packet->size += aligned;

	return 0;
}

static int
Packet_AddCallback( Packet			*packet,
				int				 msg_id,
				MessageCallbackFunc	 func,
				void 			*ctx,
				int				 param )
{
	MessageCallback *callback;

	FUSION_DEBUG( "%s( %p )\n", __FUNCTION__, packet );

	D_MAGIC_ASSERT( packet, Packet );

	callback = kmalloc( sizeof(MessageCallback), GFP_ATOMIC );
	if (!callback)
		return -ENOMEM;

	callback->msg_id = msg_id;
	callback->func   = func;
	callback->ctx    = ctx;
	callback->param  = param;

	fusion_fifo_put( &packet->callbacks, &callback->link );

	return 0;
}

static int
Packet_RunCallbacks( FusionDev *dev,
				 Packet    *packet )
{
	MessageCallback *callback;

	FUSION_DEBUG( "%s( %p )\n", __FUNCTION__, packet );

	D_MAGIC_ASSERT( packet, Packet );

	while ((callback = (MessageCallback *) fusion_fifo_get(&packet->callbacks)) != NULL) {
		D_MAGIC_ASSERT( packet, Packet );

		if (callback->func)
			callback->func( dev, callback->msg_id, callback->ctx, callback->param );

		kfree( callback );
	}

	return 0;
}

static bool
Packet_Search( Packet			*packet,
			FusionMessageType	 msg_type,
			int				 msg_id )
{
	char   *buf = packet->buf;
	size_t  pos = 0;

	FUSION_DEBUG( "%s( %p )\n", __FUNCTION__, packet );

	D_MAGIC_ASSERT( packet, Packet );

	while (pos < packet->size) {
		FusionReadMessage *header = (FusionReadMessage *) &buf[pos];

		if (header->msg_type == msg_type && header->msg_id == msg_id)
			return true;

		pos += sizeof(FusionReadMessage) + ((header->msg_size + 3) & ~3);
	}

	return false;
}

/******************************************************************************/

static int
Fusionee_GetPacket( Fusionee  *fusionee,
				size_t     size,
				Packet   **ret_packet )
{
	Packet *packet;

	FUSION_DEBUG( "%s( %p )\n", __FUNCTION__, fusionee );

	FUSION_ASSERT( size <= FUSION_MAX_PACKET_SIZE );

	packet = (Packet*) direct_list_last( fusionee->packets.items );

	D_MAGIC_ASSERT_IF( packet, Packet );

	if (!packet || packet->size + size > FUSION_MAX_PACKET_SIZE) {
		if (packet) {
			packet->flush = true;

			wake_up_interruptible_all(&fusionee->wait_receive);
		}

		if (fusionee->free_packets.count) {
			packet = (Packet*) fusion_fifo_get( &fusionee->free_packets );

			D_MAGIC_ASSERT( packet, Packet );
		}
		else
			packet = Packet_New();
		if (!packet)
			return -ENOMEM;

		D_ASSERT( packet->link.prev == NULL );
		D_ASSERT( packet->link.next == NULL );

		fusion_fifo_put( &fusionee->packets, &packet->link );
	}

	D_MAGIC_ASSERT( packet, Packet );

	*ret_packet = packet;

	return 0;
}

static void
Fusionee_PutPacket( Fusionee *fusionee,
				Packet   *packet )
{
	FUSION_DEBUG( "%s( %p )\n", __FUNCTION__, fusionee );

	D_MAGIC_ASSERT( packet, Packet );

	D_ASSERT( packet->link.prev == NULL );
	D_ASSERT( packet->link.next == NULL );

	if (fusionee->free_packets.count > 10)
		Packet_Free( packet );
	else {
		packet->size  = 0;
		packet->flush = false;

		fusion_fifo_reset( &packet->callbacks );

		fusion_fifo_put( &fusionee->free_packets, &packet->link );
	}
}

/******************************************************************************/

static int lookup_fusionee(FusionDev * dev, FusionID id,
			   Fusionee ** ret_fusionee);
static int lock_fusionee(FusionDev * dev, FusionID id,
			 Fusionee ** ret_fusionee);
static void unlock_fusionee(Fusionee * fusionee);

static void flush_packets(Fusionee *fusionee, FusionDev * dev, FusionFifo * fifo);
static void free_packets(Fusionee *fusionee, FusionDev * dev, FusionFifo * fifo);

/******************************************************************************/

static int
fusionees_read_proc(char *buf, char **start, off_t offset,
		    int len, int *eof, void *private)
{
	Fusionee *fusionee;
	FusionDev *dev = private;
	int written = 0;

	if (down_interruptible(&dev->fusionee.lock))
		return -EINTR;

	direct_list_foreach(fusionee, dev->fusionee.list) {
		written +=
		    sprintf(buf + written,
			    "(%5d) 0x%08lx (%4d packets waiting, %7ld received, %7ld sent)\n",
			    fusionee->pid, fusionee->id,
			    fusionee->packets.count, atomic_long_read(&fusionee->rcv_total),
			    atomic_long_read(&fusionee->snd_total));
		if (written < offset) {
			offset -= written;
			written = 0;
		}

		if (written >= len)
			break;
	}

	up(&dev->fusionee.lock);

	*start = buf + offset;
	written -= offset;
	if (written > len) {
		*eof = 0;
		return len;
	}

	*eof = 1;
	return (written < 0) ? 0 : written;
}

int fusionee_init(FusionDev * dev)
{
	init_waitqueue_head(&dev->fusionee.wait);

	sema_init(&dev->fusionee.lock, 1);

	create_proc_read_entry("fusionees", 0, dev->proc_dir,
			       fusionees_read_proc, dev);

	return 0;
}

void fusionee_deinit(FusionDev * dev)
{
	Fusionee *fusionee, *next;

	down(&dev->fusionee.lock);

	remove_proc_entry("fusionees", dev->proc_dir);

	direct_list_foreach_safe (fusionee, next, dev->fusionee.list) {
		while (fusionee->packets.count) {
			Packet *packet = (Packet *) fusion_fifo_get(&fusionee->packets);

			Packet_Free( packet );
		}

		kfree(fusionee);
	}

	up(&dev->fusionee.lock);
}

/******************************************************************************/

int fusionee_new(FusionDev * dev, bool force_slave, Fusionee ** ret_fusionee)
{
	Fusionee *fusionee;

	fusionee = kmalloc(sizeof(Fusionee), GFP_ATOMIC);
	if (!fusionee)
		return -ENOMEM;

	memset(fusionee, 0, sizeof(Fusionee));

	if (down_interruptible(&dev->fusionee.lock)) {
		kfree(fusionee);
		return -EINTR;
	}

	fusionee->refs = 1;

	fusionee->pid = current->pid;
	fusionee->force_slave = force_slave;
	fusionee->mm = current->mm;

	spin_lock_init( &fusionee->lock );

	init_waitqueue_head(&fusionee->wait_receive);
	init_waitqueue_head(&fusionee->wait_process);

	direct_list_prepend(&dev->fusionee.list, &fusionee->link);

	up(&dev->fusionee.lock);

	fusionee->fusion_dev = dev;

	*ret_fusionee = fusionee;

	return 0;
}

int fusionee_enter(FusionDev * dev, FusionEnter * enter, Fusionee * fusionee)
{
	if (down_interruptible(&dev->enter_lock))
		return -EINTR;

	if (dev->fusionee.last_id || fusionee->force_slave) {
		while (!dev->enter_ok) {
			fusion_sleep_on(&dev->enter_wait, &dev->enter_lock,
					NULL);

			if (signal_pending(current))
				return -EINTR;

			if (down_interruptible(&dev->enter_lock))
				return -EINTR;
		}

		FUSION_ASSERT(dev->fusionee.last_id != 0);
	}

	if (dev->fusionee.last_id == 0) {
		/* master determines Fusion API (if supported) */
		int major = enter->api.major;
		if ((major != 3) && (major != 4) && (major != 8))
			return -ENOPROTOOPT;

		dev->api.major = enter->api.major;
		dev->api.minor = enter->api.minor;
	} else {
		if ((enter->api.major != dev->api.major)
		    || (enter->api.minor > dev->api.minor))
			return -ENOPROTOOPT;
	}

	fusionee->id = ++dev->fusionee.last_id;

	up(&dev->enter_lock);

	enter->fusion_id = fusionee->id;

	return 0;
}

int fusionee_fork(FusionDev * dev, FusionFork * fork, Fusionee * fusionee)
{
	int ret;

	ret = fusion_shmpool_fork_all(dev, fusionee->id, fork->fusion_id);
	if (ret)
		return ret;

	ret = fusion_reactor_fork_all(dev, fusionee->id, fork->fusion_id);
	if (ret)
		return ret;

	ret = fusion_ref_fork_all_local(dev, fusionee->id, fork->fusion_id);
	if (ret)
		return ret;

	fork->fusion_id = fusionee->id;

	return 0;
}

int
fusionee_send_message(FusionDev * dev,
		      Fusionee * sender,
		      FusionID recipient,
		      FusionMessageType msg_type,
		      int msg_id,
		      int msg_channel,
		      int msg_size,
		      const void *msg_data,
		      MessageCallbackFunc callback,
		      void *callback_ctx, int callback_param,
			 const void *extra_data, unsigned int extra_size )
{
	int	                    ret;
	Packet                  *packet;
	Fusionee                *fusionee;
	size_t                   size;
	const FusionCallMessage *call = msg_data;

	ret = lookup_fusionee(dev, recipient, &fusionee);
	if (ret)
		return ret;

	FUSION_DEBUG("fusionee_send_message (%d -> %d, type %d, id %d, size %d, extra %d)\n",
		 fusionee->id, recipient, msg_type, msg_id, msg_size, extra_size);

again:
	spin_lock(&fusionee->lock);

	if (fusionee->packets.count > 10) {
		up(&dev->fusionee.lock);

		fusion_sleep_on_spinlock(&fusionee->wait_process, &fusionee->lock, 0);

		if (signal_pending(current))
			return -EINTR;

		goto again;
	}

	if (sender && sender != fusionee)
		spin_lock( &sender->lock );

	up(&dev->fusionee.lock);

	ret = Fusionee_GetPacket( fusionee, sizeof(FusionReadMessage) + msg_size + extra_size, &packet );
	if (ret)
		goto error;

	D_MAGIC_ASSERT( packet, Packet );


	/* keep size for error handling, the other way round we'd need to remove the callback :( */
	size = packet->size;

	ret = Packet_Write( packet, msg_type, msg_id, msg_channel,
					msg_data, msg_size, extra_data, extra_size,
					msg_type != FMT_CALL && msg_type != FMT_SHMPOOL );
	if (ret)
		goto error;



	D_MAGIC_ASSERT( packet, Packet );

	if (callback) {
		ret = Packet_AddCallback( packet, msg_id, callback, callback_ctx, callback_param );
		if (ret) {
			packet->size = size;
			goto error;
		}
	}


	atomic_long_inc(&fusionee->rcv_total);
	if (sender)
		atomic_long_inc(&sender->snd_total);


	if (msg_type != FMT_CALL || call->serial || !sender) {
		packet->flush = true;

		wake_up_interruptible_all(&fusionee->wait_receive);
	}


	if (sender && sender != fusionee)
		unlock_fusionee(sender);

	unlock_fusionee(fusionee);


	return 0;


error:
	if (sender && sender != fusionee)
		unlock_fusionee(sender);

	unlock_fusionee(fusionee);

	return ret;
}

int
fusionee_send_message2(FusionDev * dev,
		      Fusionee *sender,
		      Fusionee *fusionee,
		      FusionMessageType msg_type,
		      int msg_id,
		      int msg_channel,
		      int msg_size,
		      const void *msg_data,
		      MessageCallbackFunc callback,
		      void *callback_ctx, int callback_param,
			 const void *extra_data, unsigned int extra_size )
{
	int	                    ret;
	Packet                  *packet;
	size_t                   size;
	const FusionCallMessage *call = msg_data;

	FUSION_DEBUG("fusionee_send_message2 (%d -> %d, type %d, id %d, size %d, extra %d)\n",
		 sender->id, fusionee->id, msg_type, msg_id, msg_size, extra_size);

again:
	spin_lock(&fusionee->lock);

	if (fusionee->packets.count > 10) {
		fusion_sleep_on_spinlock(&fusionee->wait_process, &fusionee->lock, 0);

		if (signal_pending(current))
			return -EINTR;

		goto again;
	}

	ret = Fusionee_GetPacket( fusionee, sizeof(FusionReadMessage) + msg_size + extra_size, &packet );
	if (ret)
		goto error;

	D_MAGIC_ASSERT( packet, Packet );


	/* keep size for error handling, the other way round we'd need to remove the callback :( */
	size = packet->size;

	ret = Packet_Write( packet, msg_type, msg_id, msg_channel,
					msg_data, msg_size, extra_data, extra_size,
					msg_type != FMT_CALL && msg_type != FMT_SHMPOOL );
	if (ret)
		goto error;



	D_MAGIC_ASSERT( packet, Packet );

	if (callback) {
		ret = Packet_AddCallback( packet, msg_id, callback, callback_ctx, callback_param );
		if (ret) {
			packet->size = size;
			goto error;
		}
	}


	atomic_long_inc(&fusionee->rcv_total);
	if (sender)
		atomic_long_inc(&sender->snd_total);


	if (msg_type != FMT_CALL || call->serial || !sender) {
		packet->flush = true;

		wake_up_interruptible_all(&fusionee->wait_receive);
	}


	unlock_fusionee(fusionee);


	return 0;


error:
	unlock_fusionee(fusionee);

	return ret;
}

int
fusionee_get_messages(FusionDev * dev,
		      Fusionee * fusionee, void *buf, int buf_size, bool block)
{
	int written = 0;
	FusionFifo prev_packets;

	FUSION_DEBUG( "%s()\n", __FUNCTION__ );

	spin_lock( &fusionee->lock );

	if (fusionee->dispatcher_pid)
		FUSION_ASSUME(fusionee->dispatcher_pid == current->pid);

	fusionee->dispatcher_pid = current->pid;

	prev_packets = fusionee->prev_packets;

	fusion_fifo_reset(&fusionee->prev_packets);

	wake_up_interruptible_all(&fusionee->wait_process);

	while (!fusionee->packets.count || !((Packet *) fusionee->packets.items)->flush) {
		if (!block) {
			unlock_fusionee(fusionee);
			flush_packets(fusionee, dev, &prev_packets);
			return -EAGAIN;
		}

		if (prev_packets.count) {
			unlock_fusionee(fusionee);
			flush_packets(fusionee, dev, &prev_packets);
		} else {
			fusion_sleep_on_spinlock(&fusionee->wait_receive, &fusionee->lock, 0);

			if (signal_pending(current))
				return -EINTR;
		}

		spin_lock( &fusionee->lock );
	}

	while (fusionee->packets.count && ((Packet *) fusionee->packets.items)->flush) {
		Packet *packet = (Packet *) fusionee->packets.items;
		int     bytes  = packet->size;

		D_MAGIC_ASSERT( packet, Packet );

		if (bytes > buf_size) {
			if (!written) {
				unlock_fusionee(fusionee);
				flush_packets(fusionee, dev, &prev_packets);
				return -EMSGSIZE;
			}

			break;
		}

		if (copy_to_user(buf, packet->buf, packet->size)) {
			unlock_fusionee(fusionee);
			flush_packets(fusionee, dev, &prev_packets);
			return -EFAULT;
		}

		written += bytes;
		buf += bytes;
		buf_size -= bytes;

		fusion_fifo_get(&fusionee->packets);

		D_MAGIC_ASSERT( packet, Packet );

		if (packet->callbacks.count)
			fusion_fifo_put(&fusionee->prev_packets, &packet->link);
		else
			Fusionee_PutPacket(fusionee, packet);
	}

	unlock_fusionee(fusionee);

	flush_packets(fusionee, dev, &prev_packets);

	return written;
}

int
fusionee_wait_processing(FusionDev * dev,
			 int fusion_id, FusionMessageType msg_type, int msg_id)
{
	Fusionee *fusionee;

     do {
		int ret;
		Packet *packet;

		ret = lock_fusionee(dev, fusion_id, &fusionee);
		if (ret)
			return ret;

		/* Search all pending packets. */
		direct_list_foreach (packet, fusionee->packets.items) {
			if (Packet_Search( packet, msg_type, msg_id ))
				break;
		}

		/* Search packets being processed right now. */
		if (!packet) {
			direct_list_foreach (packet, fusionee->prev_packets.items) {
				if (Packet_Search( packet, msg_type, msg_id ))
					break;
			}
		}

		/* Really no more packet of that type and ID? */
		if (!packet)
			break;

		if (fusionee->dispatcher_pid)
			FUSION_ASSUME(fusionee->dispatcher_pid != current->pid);

		/* Otherwise unlock and wait. */
		fusion_sleep_on_spinlock(&fusionee->wait_process, &fusionee->lock, 0);

		if (signal_pending(current))
			return -EINTR;
	} while (true);

	unlock_fusionee(fusionee);

	return 0;
}

unsigned int
fusionee_poll(FusionDev * dev,
	      Fusionee * fusionee, struct file *file, poll_table * wait)
{
	int ret;
	FusionID id = fusionee->id;
	FusionFifo prev_msgs;

	ret = lock_fusionee(dev, id, &fusionee);
	if (ret)
		return POLLERR;

	prev_msgs = fusionee->prev_packets;

	fusion_fifo_reset(&fusionee->prev_packets);

	unlock_fusionee(fusionee);

	flush_packets(fusionee, dev, &prev_msgs);

	wake_up_all(&fusionee->wait_process);

	poll_wait(file, &fusionee->wait_receive, wait);

	ret = lock_fusionee(dev, id, &fusionee);
	if (ret)
		return POLLERR;

	if (fusionee->packets.count && ((Packet *) fusionee->packets.items)->flush) {
		unlock_fusionee(fusionee);

		return POLLIN | POLLRDNORM;
	}

	unlock_fusionee(fusionee);

	return 0;
}

int
fusionee_kill(FusionDev * dev,
	      Fusionee * fusionee, FusionID target, int signal, int timeout_ms)
{
	long timeout = -1;

	while (true) {
		Fusionee *f;
		int killed = 0;

		if (down_interruptible(&dev->fusionee.lock))
			return -EINTR;

		direct_list_foreach(f, dev->fusionee.list) {
			if (f != fusionee && (!target || target == f->id)) {
				struct task_struct *p;

#if defined(CONFIG_TREE_RCU) || defined(CONFIG_TREE_PREEMPT_RCU) || defined(CONFIG_TINY_RCU) || defined(rcu_read_lock)
				rcu_read_lock();
#else
				read_lock(&tasklist_lock);
#endif

#ifdef for_each_task		/* 2.4 */
				for_each_task(p) {
#else /* for >= 2.6.0 & redhat WS EL3 w/ 2.4 kernel */
				for_each_process(p) {
#endif
					if (p->mm == f->mm) {
						send_sig_info(signal,
							      (void *)1L
							      /* 1 means from kernel */
							      ,
							      p);
						killed++;
					}
				}

#if defined(CONFIG_TREE_RCU) || defined(CONFIG_TREE_PREEMPT_RCU) || defined(CONFIG_TINY_RCU) || defined(rcu_read_unlock)
				rcu_read_unlock();
#else
				read_unlock(&tasklist_lock);
#endif
			}
		}

		if (!killed || timeout_ms < 0) {
			up(&dev->fusionee.lock);
			break;
		}

		if (timeout_ms) {
			switch (timeout) {
			case 0:	/* timed out */
				up(&dev->fusionee.lock);
				return -ETIMEDOUT;

			case -1:	/* setup timeout */
				timeout = (timeout_ms * HZ + 500) / 1000;
				if (!timeout)
					timeout = 1;

				/* fall through */

			default:
				fusion_sleep_on(&dev->fusionee.wait,
						&dev->fusionee.lock, &timeout);
				break;
			}
		} else
			fusion_sleep_on(&dev->fusionee.wait,
					&dev->fusionee.lock, NULL);

		if (signal_pending(current))
			return -EINTR;
	}

	return 0;
}

void
fusionee_ref( Fusionee * fusionee )
{
	FUSION_ASSERT( fusionee != NULL );

	spin_lock( &fusionee->lock );

	FUSION_ASSERT( fusionee->refs > 0 );

	fusionee->refs++;

	spin_unlock( &fusionee->lock );
}

void
fusionee_unref( Fusionee * fusionee )
{
	FUSION_ASSERT( fusionee != NULL );

	spin_lock( &fusionee->lock );

	FUSION_ASSERT( fusionee->refs > 0 );

	if (!--fusionee->refs)
		kfree( fusionee );
	else
		spin_unlock( &fusionee->lock );
}


void fusionee_destroy(FusionDev * dev, Fusionee * fusionee)
{
	FusionFifo prev_packets;
	FusionFifo packets;

	FUSION_ASSERT( fusionee != NULL );
	FUSION_ASSERT( fusionee->refs > 0 );

	/* Lock list. */
	down(&dev->fusionee.lock);

	/* Lock fusionee. */
	spin_lock(&fusionee->lock);

	prev_packets = fusionee->prev_packets;
	packets      = fusionee->packets;

	/* Remove from list. */
	direct_list_remove(&dev->fusionee.list, &fusionee->link);

	/* Wake up waiting killer. */
	wake_up_interruptible_all(&dev->fusionee.wait);

	/* Unlock list. */
	up(&dev->fusionee.lock);

	/* Release locks, references, ... */
	fusion_skirmish_dismiss_all(dev, fusionee->id);
	fusion_skirmish_return_all_from(dev, fusionee->id);
	fusion_call_destroy_all(dev, fusionee);
	fusion_reactor_detach_all(dev, fusionee->id);
	fusion_property_cede_all(dev, fusionee->id);
	fusion_ref_clear_all_local(dev, fusionee->id);
	fusion_shmpool_detach_all(dev, fusionee->id);

	/* Unlock fusionee. */
	spin_unlock(&fusionee->lock);

	/* Free all pending messages. */
	flush_packets(fusionee, dev, &prev_packets);
	flush_packets(fusionee, dev, &packets);

	free_packets(fusionee, dev, &fusionee->free_packets);

	/* Free fusionee data. */
	fusionee_unref( fusionee );
}

FusionID fusionee_id(const Fusionee * fusionee)
{
	return fusionee->id;
}

pid_t fusionee_dispatcher_pid(FusionDev * dev, FusionID fusion_id)
{
	Fusionee *fusionee;
	int ret = -EINVAL;

	down(&dev->fusionee.lock);

	direct_list_foreach(fusionee, dev->fusionee.list) {
		if (fusionee->id == fusion_id) {
			/* FIXME: wait for it? */
			FUSION_ASSUME(fusionee->dispatcher_pid != 0);

			ret = fusionee->dispatcher_pid;
			break;
		}
	}

	up(&dev->fusionee.lock);

	return ret;
}

/******************************************************************************/

static int
lookup_fusionee(FusionDev * dev, FusionID id, Fusionee ** ret_fusionee)
{
	Fusionee *fusionee;

	down(&dev->fusionee.lock);

	direct_list_foreach(fusionee, dev->fusionee.list) {
		if (fusionee->id == id) {
			*ret_fusionee = fusionee;
			return 0;
		}
	}

	up(&dev->fusionee.lock);

	return -EINVAL;
}

static int lock_fusionee(FusionDev * dev, FusionID id, Fusionee ** ret_fusionee)
{
	int ret;
	Fusionee *fusionee;

	ret = lookup_fusionee(dev, id, &fusionee);
	if (ret)
		return ret;

	direct_list_move_to_front(&dev->fusionee.list, &fusionee->link);

	spin_lock(&fusionee->lock);

	up(&dev->fusionee.lock);

	*ret_fusionee = fusionee;

	return 0;
}

static void unlock_fusionee(Fusionee * fusionee)
{
	spin_unlock(&fusionee->lock);
}

/******************************************************************************/

static void flush_packets(Fusionee *fusionee, FusionDev * dev, FusionFifo * fifo)
{
	Packet *packet;

	while ((packet = (Packet *) fusion_fifo_get(fifo)) != NULL) {
		D_MAGIC_ASSERT( packet, Packet );

		Packet_RunCallbacks( dev, packet );

		Fusionee_PutPacket( fusionee, packet );
	}
}

static void free_packets(Fusionee *fusionee, FusionDev * dev, FusionFifo * fifo)
{
	Packet *packet;

	while ((packet = (Packet *) fusion_fifo_get(fifo)) != NULL) {
		D_MAGIC_ASSERT( packet, Packet );

		Packet_Free( packet );
	}
}
