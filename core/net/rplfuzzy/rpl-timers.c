/**
 * \addtogroup uip6
 * @{
 */
/*
 * Copyright (c) 2010, Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 */
/**
 * \file
 *         RPL timer management.
 *
 * \author Joakim Eriksson <joakime@sics.se>, Nicolas Tsiftes <nvt@sics.se>
 */

#include "contiki-conf.h"
#include "net/rplfuzzy/rpl-private.h"
#include "net/rplfuzzy/fuzzify.h"
#include "lib/random.h"
#include "sys/ctimer.h"

#include "sys/clock.h"


#define DEBUG DEBUG_NONE
#include "net/uip-debug.h"

/************************************************************************/
static struct ctimer periodic_timer;

static void handle_periodic_timer(void *ptr);
static void new_dio_interval(rpl_dag_t *dag);
static void handle_dio_timer(void *ptr);

static uint16_t next_dis;

/* dio_send_ok is true if the node is ready to send DIOs */
static uint8_t dio_send_ok;


/************************************************************************/
static void
handle_periodic_timer(void *ptr)
{
  rpl_purge_routes();
  rpl_recalculate_ranks();

  /* handle DIS */
#ifdef RPL_DIS_SEND
  next_dis++;
  if(rpl_get_dag(RPL_ANY_INSTANCE) == NULL && next_dis >= RPL_DIS_INTERVAL) {
    next_dis = 0;
    dis_output(NULL);
  }
#endif
  ctimer_reset(&periodic_timer);
}
/************************************************************************/

static uint32_t next_time = 0;
static uint32_t next_delay = 0;

new_interval(uint32_t * time, uint32_t * next){
  *time = (*time * CLOCK_SECOND)/1000;
  *next = *time;
  *time = *time >> 1;
  *time += (*time * random_rand()) / RANDOM_RAND_MAX;
  *next -= *time;
}

/************************************************************************/
static void
new_dio_interval(rpl_dag_t *dag)
{
  uint32_t time;

  if (!next_time){
  /* TODO: too small timer intervals for many cases */
     time = 1UL << dag->dio_intcurrent;

     new_interval(&time, &dag->dio_next_delay);
  } else {
    time = next_time;
    dag->dio_next_delay = next_delay;
  }
  dag->dio_send = 1;
  
#if RPL_CONF_STATS
  /* keep some stats */
  dag->dio_totint++;
  dag->dio_totrecv += dag->dio_counter;
  ANNOTATE("#A rank=%u.%u(%u),stats=%d %d %d %d,color=%s\n",
	   DAG_RANK(dag->rank, dag),
           (10 * (dag->rank % dag->min_hoprankinc)) / dag->min_hoprankinc,
           dag->version,
           dag->dio_totint, dag->dio_totsend,
           dag->dio_totrecv,dag->dio_intcurrent,
	   dag->rank == ROOT_RANK(dag) ? "BLUE" : "ORANGE");
#endif /* RPL_CONF_STATS */
  
  /* reset the redundancy counter */
  dag->dio_counter = 0;
  
  /* schedule the timer */
  PRINTF("RPL: Scheduling DIO timer %lu ticks in future (Interval)\n", time);
  ctimer_set(&dag->dio_timer, time, &handle_dio_timer, dag);

  /* schedule the next next dio output */

  next_time = 1UL << (dag->dio_intcurrent+1);
  new_interval(&next_time, &next_delay);
  dio_output_set_next(next_time, next_delay,dag->dio_next_delay);
}
/************************************************************************/
static void
handle_dio_timer(void *ptr)
{
  rpl_dag_t *dag;
  PRINTF("RPL: handle dio timer\n");
  dag = (rpl_dag_t *)ptr; 

  if(!dio_send_ok) {
    if(uip_ds6_get_link_local(ADDR_PREFERRED) != NULL) {
      dio_send_ok = 1;
    } else {
      PRINTF("RPL: Postponing DIO transmission since link local address is not ok\n");
      ctimer_set(&dag->dio_timer, CLOCK_SECOND, &handle_dio_timer, dag);
      return;
    }
  }

  if(dag->dio_send) {
    /* send DIO if counter is less than desired redundancy */
    if(dag->dio_counter < dag->dio_redundancy) {
#if RPL_CONF_STATS
      dag->dio_totsend++;
#endif /* RPL_CONF_STATS */
      dio_output(dag, NULL);
    } else {
      PRINTF("RPL: Supressing DIO transmission (%d >= %d)\n",
           dag->dio_counter, dag->dio_redundancy);
    }
    dag->dio_send = 0;
    PRINTF("RPL: Scheduling DIO timer %lu ticks in future (sent)\n", dag->dio_next_delay);
    ctimer_set(&dag->dio_timer, dag->dio_next_delay, handle_dio_timer, dag);
  } else {
    /* check if we need to double interval */
    if(dag->dio_intcurrent < dag->dio_intmin + dag->dio_intdoubl) {
      dag->dio_intcurrent++;
    }
    new_dio_interval(dag);

  }
}
/************************************************************************/
void
rpl_reset_periodic_timer(void)
{
  next_dis = RPL_DIS_INTERVAL - RPL_DIS_START_DELAY;
  ctimer_set(&periodic_timer, CLOCK_SECOND, handle_periodic_timer, NULL);
}
/************************************************************************/
/* Resets the DIO timer in the DAG to its minimal interval. */
void
rpl_reset_dio_timer(rpl_dag_t *dag, uint8_t force)
{
    /* only reset if not just reset or started */
  if(force || dag->dio_intcurrent > dag->dio_intmin) {
    PRINTF("RPL: Reset DIO Timer\n");
    dag->dio_counter = 0;
    dag->dio_intcurrent = dag->dio_intmin;

    /* FUZZY LATENCY */
    next_time = 0;
    /* FUZZY LATENCY */

    new_dio_interval(dag);
  }
#if RPL_CONF_STATS
  rpl_stats.resets++;
#endif
}
/************************************************************************/
static void
handle_dao_timer(void *ptr)
{
  rpl_dag_t *dag;

  dag = (rpl_dag_t *)ptr;

  if (!dio_send_ok && uip_ds6_get_link_local(ADDR_PREFERRED) == NULL) {
    PRINTF("RPL: Postpone DAO transmission... \n");
    ctimer_set(&dag->dao_timer, CLOCK_SECOND, handle_dao_timer, dag);
    return;
  }

  /* Send the DAO to the DAO parent set -- the preferred parent in our case. */
  if(dag->preferred_parent != NULL) {
    PRINTF("RPL: handle_dao_timer - sending DAO\n");
    /* Set the route lifetime to the default value. */
    dao_output(dag->preferred_parent, dag->default_lifetime);
  } else {
    PRINTF("RPL: No suitable DAO parent\n");
  }
  ctimer_stop(&dag->dao_timer);
}
/************************************************************************/
void
rpl_schedule_dao(rpl_dag_t *dag)
{
  clock_time_t expiration_time;

  expiration_time = etimer_expiration_time(&dag->dao_timer.etimer);

  if(!etimer_expired(&dag->dao_timer.etimer)) {
    PRINTF("RPL: DAO timer already scheduled\n");
  } else {
    expiration_time = DEFAULT_DAO_LATENCY / 2 +
      (random_rand() % (DEFAULT_DAO_LATENCY));
    PRINTF("RPL: Scheduling DAO timer %u ticks in the future\n",
          (unsigned)expiration_time);
    ctimer_set(&dag->dao_timer, expiration_time,
               handle_dao_timer, dag);
  }
}
/************************************************************************/
void handle_next_dio(void * ptr);

void rpl_schedule_next_dio_reception(rpl_parent_t * p, uint32_t delay, uint32_t next_time, uint32_t next_delay){
  if(p->dag->preferred_parent == p){
    uint32_t now = clock_time();
    p->next_dio_delay = next_delay;
    p->next_dio_time = next_time;
    p->next_dio_start_interval = now + delay;
    ctimer_set(&p->latency_timer, delay + next_time + next_delay ,handle_next_dio, p);
    ANNOTATE("RPL:TRIGGER:DIO it is %lu, next DIO waited at %lu until %lu\n", 
	     now,
	     p->next_dio_start_interval + next_time, 
	     p->next_dio_start_interval + next_time + next_delay);
  }
}

static int dis_sended;

static struct ctimer dis_timer;

void send_dis(void * ptr){
  rpl_parent_t * p = (rpl_parent_t *)ptr;
  if (!p->first_dio_received){
    ANNOTATE("RPL:TRIGGER:DIO Send DIS to %u %lu\n",p->addr.u8[15],(100000 * random_rand()) / RANDOM_RAND_MAX);
    dis_output(p->addr.u8);
    //dis_output(NULL);
    ctimer_set(&dis_timer, (100000 * random_rand()) / RANDOM_RAND_MAX, send_dis, p);
  }
}


void handle_next_dio(void * ptr){
  rpl_parent_t * p = (rpl_parent_t *)ptr;
  if(p->dag->preferred_parent == p){
    p->first_dio_received = 0;
    ANNOTATE("RPL:TRIGGER:DIO is is %lu and no DIO received : RAZ\n",clock_time());
    /* p->next_dio_start_interval = clock_time(); */
    /* p->next_dio_time += p->next_dio_time ; */
    /* p->next_dio_delay += p->next_dio_delay; */
    /* ANNOTATE("RPL:TRIGGER:DIO non recu at %lu. Wait next DIO at %lu until %lu\n",  */
    /* 	     p->next_dio_start_interval, */
    /* 	     p->next_dio_start_interval + p->next_dio_time, */
    /* 	     p->next_dio_start_interval + p->next_dio_time  + p->next_dio_delay); */
    //ctimer_set(&p->latency_timer, p->next_dio_time + p->next_dio_delay,handle_next_dio, p);

    ctimer_set(&dis_timer, (100000 * random_rand()) / RANDOM_RAND_MAX, send_dis, p);
    /* send_dis(p->addr.u8); */
    /* dis_sended = 1; */
  }
}

void rpl_dio_received(rpl_parent_t * p, uint32_t delay, uint32_t next_time, uint32_t next_delay){
  if(p->dag->preferred_parent == p){
    next_delay += 400;
    if (!p->first_dio_received){
      ANNOTATE("RPL:TRIGGER:DIO first recu delay %lu, next time %lu, next delay %lu\n",
	       delay, 
	       next_time, 
	       next_delay);
      rpl_schedule_next_dio_reception(p,delay, next_time, next_delay);
      p->first_dio_received = 1;
      dis_sended = 0;
    }
    else {
      uint32_t now = clock_time();

      if (dis_sended){

	uint8_t residual = p->next_dio_start_interval + p->next_dio_time + p->next_dio_delay + next_time - now;
	ctimer_set(&p->latency_timer,residual + next_delay, handle_next_dio, p);
	p->next_dio_start_interval = p->next_dio_start_interval;
	p->next_dio_time = next_time;
	p->next_dio_delay = next_delay;
	dis_sended = 0;
      }	   

      if (now > (p->next_dio_start_interval + p->next_dio_time)){
	p->latency_metric = now - (p->next_dio_start_interval + p->next_dio_time);
	ANNOTATE("RPL:TRIGGER:DIO recu at %lu. Latency = %lu\n",now, p->latency_metric);
	p->dag->of->update_metric_container(p->dag);
	rpl_schedule_next_dio_reception(p, delay, next_time, next_delay);
      }
      else {
	if ((next_time + next_delay) == (p->next_dio_time + p->next_dio_delay)){
	  ANNOTATE("RPL:TRIGGER:DIO recu at %lu < %lu + %lu. Latency too early  de %lu\n",
		   now,
		   p->next_dio_start_interval,
		   p->next_dio_time,
		   (p->next_dio_time + p->next_dio_start_interval) - now );
	  rpl_schedule_next_dio_reception(p, delay, next_time, next_delay);
	}
      }
    }
  }
}


