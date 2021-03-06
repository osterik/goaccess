/**
 * gdns.c -- hosts resolver
 * Copyright (C) 2009-2014 by Gerardo Orellana <goaccess@prosoftcorp.com>
 * GoAccess - An Ncurses apache weblog analyzer & interactive viewer
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * A copy of the GNU General Public License is attached to this
 * source distribution for its full text.
 *
 * Visit http://goaccess.prosoftcorp.com for new releases.
 */

#define _MULTI_THREADED
#ifdef __FreeBSD__
#include <sys/socket.h>
#endif

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "gdns.h"

#ifdef HAVE_LIBTOKYOCABINET
#include "tcabdb.h"
#else
#include "gkhash.h"
#endif

#include "error.h"
#include "goaccess.h"
#include "util.h"
#include "xmalloc.h"

GDnsThread gdns_thread;
static GDnsQueue *gdns_queue;

/* Initialize the queue. */
void
gqueue_init (GDnsQueue * q, int capacity)
{
  q->head = 0;
  q->tail = -1;
  q->size = 0;
  q->capacity = capacity;
}

/* Get the current size of queue.
 *
 * Returns the size of the queue. */
int
gqueue_size (GDnsQueue * q)
{
  return q->size;
}

/* Determine if the queue is empty.
 *
 * Returns true if empty, otherwise false. */
int
gqueue_empty (GDnsQueue * q)
{
  return q->size == 0;
}

/* Determine if the queue is full.
 *
 * Returns true if full, otherwise false. */
int
gqueue_full (GDnsQueue * q)
{
  return q->size == q->capacity;
}

/* Free the queue. */
void
gqueue_destroy (GDnsQueue * q)
{
  free (q);
}

/* Add at the end of the queue a string item.
 *
 * If the queue is full, -1 is returned.
 * If added to the queue, 0 is returned. */
int
gqueue_enqueue (GDnsQueue * q, char *item)
{
  if (gqueue_full (q))
    return -1;

  q->tail = (q->tail + 1) % q->capacity;
  strcpy (q->buffer[q->tail], item);
  q->size++;
  return 0;
}

/* Find a string item in the queue.
 *
 * If the queue is empty, or the item is not in the queue, 0 is returned.
 * If found, 1 is returned. */
int
gqueue_find (GDnsQueue * q, const char *item)
{
  int i;
  if (gqueue_empty (q))
    return 0;

  for (i = 0; i < q->size; i++) {
    if (strcmp (item, q->buffer[i]) == 0)
      return 1;
  }
  return 0;
}

/* Remove a string item from the head of the queue.
 *
 * If the queue is empty, NULL is returned.
 * If removed, the string item is returned. */
char *
gqueue_dequeue (GDnsQueue * q)
{
  char *item;
  if (gqueue_empty (q))
    return NULL;

  item = q->buffer[q->head];
  q->head = (q->head + 1) % q->capacity;
  q->size--;
  return item;
}

/* Get the corresponding hostname given an IP address.
 *
 * On error, a string error message is returned.
 * On success, a malloc'd hostname is returned. */
static char *
reverse_host (const struct sockaddr *a, socklen_t length)
{
  char h[H_SIZE];
  int flags, st;

  flags = NI_NAMEREQD;
  st = getnameinfo (a, length, h, H_SIZE, NULL, 0, flags);
  if (!st)
    return alloc_string (h);
  return alloc_string (gai_strerror (st));
}

/* Determine if IPv4 or IPv6 and resolve.
 *
 * On error, NULL is returned.
 * On success, a malloc'd hostname is returned. */
char *
reverse_ip (char *str)
{
  union
  {
    struct sockaddr addr;
    struct sockaddr_in6 addr6;
    struct sockaddr_in addr4;
  } a;

  if (str == NULL || *str == '\0')
    return NULL;

  memset (&a, 0, sizeof (a));
  if (1 == inet_pton (AF_INET, str, &a.addr4.sin_addr)) {
    a.addr4.sin_family = AF_INET;
    return reverse_host (&a.addr, sizeof (a.addr4));
  } else if (1 == inet_pton (AF_INET6, str, &a.addr6.sin6_addr)) {
    a.addr6.sin6_family = AF_INET6;
    return reverse_host (&a.addr, sizeof (a.addr6));
  }
  return NULL;
}

/* Producer - Resolve an IP address and add it to the queue. */
void
dns_resolver (char *addr)
{
  pthread_mutex_lock (&gdns_thread.mutex);
  /* queue is not full and the IP address is not in the queue */
  if (!gqueue_full (gdns_queue) && !gqueue_find (gdns_queue, addr)) {
    /* add the IP to the queue */
    gqueue_enqueue (gdns_queue, addr);
    pthread_cond_broadcast (&gdns_thread.not_empty);
  }
  pthread_mutex_unlock (&gdns_thread.mutex);
}

/* Consumer - Once an IP has been resolved, add it to dwithe hostnames
 * hash structure. */
static void
dns_worker (void GO_UNUSED (*ptr_data))
{
  char *ip = NULL, *host = NULL;

  while (1) {
    pthread_mutex_lock (&gdns_thread.mutex);
    /* wait until an item has been added to the queue */
    while (gqueue_empty (gdns_queue))
      pthread_cond_wait (&gdns_thread.not_empty, &gdns_thread.mutex);

    ip = gqueue_dequeue (gdns_queue);

    pthread_mutex_unlock (&gdns_thread.mutex);
    host = reverse_ip (ip);
    pthread_mutex_lock (&gdns_thread.mutex);

    if (!active_gdns) {
      if (host)
        free (host);
      break;
    }

    /* insert the corresponding IP -> hostname map */
    if (host != NULL && active_gdns) {
      ht_insert_hostname (ip, host);
      free (host);
    }

    pthread_cond_signal (&gdns_thread.not_full);
    pthread_mutex_unlock (&gdns_thread.mutex);
  }
}

/* Initialize queue and dns thread */
void
gdns_init (void)
{
  gdns_queue = xmalloc (sizeof (GDnsQueue));
  gqueue_init (gdns_queue, QUEUE_SIZE);

  if (pthread_cond_init (&(gdns_thread.not_empty), NULL))
    FATAL ("Failed init thread condition");

  if (pthread_cond_init (&(gdns_thread.not_full), NULL))
    FATAL ("Failed init thread condition");

  if (pthread_mutex_init (&(gdns_thread.mutex), NULL))
    FATAL ("Failed init thread mutex");
}

/* Destroy (free) queue */
void
gdns_free_queue (void)
{
  gqueue_destroy (gdns_queue);
}

/* Create a DNS thread and make it active */
void
gdns_thread_create (void)
{
  int thread;

  active_gdns = 1;
  thread =
    pthread_create (&(gdns_thread.thread), NULL, (void *) &dns_worker, NULL);
  if (thread)
    FATAL ("Return code from pthread_create(): %d", thread);
  pthread_detach (gdns_thread.thread);
}
