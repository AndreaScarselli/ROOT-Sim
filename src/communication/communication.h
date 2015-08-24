/**
*			Copyright (C) 2008-2015 HPDCS Group
*			http://www.dis.uniroma1.it/~hpdcs
*
*
* This file is part of ROOT-Sim (ROme OpTimistic Simulator).
*
* ROOT-Sim is free software; you can redistribute it and/or modify it under the
* terms of the GNU General Public License as published by the Free Software
* Foundation; either version 3 of the License, or (at your option) any later
* version.
*
* ROOT-Sim is distributed in the hope that it will be useful, but WITHOUT ANY
* WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
* A PARTICULAR PURPOSE. See the GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with
* ROOT-Sim; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*
* @file communication.h
* @brief This is the main header file, containing all data structures and defines
*        used by the communication subsystem.
* @author Francesco Quaglia
* @author Roberto Vitali
*
* @todo There are still some defines undocumented
*/

#pragma once
#ifndef _COMMUNICATION_H_
#define _COMMUNICATION_H_

#include <core/core.h>


/// Ack window size
#define WND_BUFFER_LENGTH	10000
/// Ack window size
#define WINDOW_DIMENSION	50



/// Number of slots used by MPI for buffering messages
#define SLOTS	100000


/// Simulation Platform Control Messages
enum _control_msgs {
	MIN_VALUE_CONTROL=65537,
	RENDEZVOUS_START,
	RENDEZVOUS_ACK,
	RENDEZVOUS_UNBLOCK,
	RENDEZVOUS_ROLLBACK,
	MAX_VALUE_CONTROL
};

// Message Codes for PVM
#define MSG_INIT_MPI		200
#define MSG_EVENT		2
#define MSG_ACKNOWLEDGE		3
#define MSG_GVT			10
#define MSG_UNLOCK		11
#define MSG_GO_TO_FINAL_BARRIER	12
#define MSG_GVT_ACK		13



// Message Codes for GVT operations
#define MSG_COMPUTE_GVT		50 /// Master asks for tables
#define MSG_INFO_GVT		51 /// Slaves reply with their information
#define MSG_NEW_GVT		52 /// Master notifies the new GVT
#define MSG_TIME_BARRIER	53 /// Slaves communicate their maximum time barrier
#define MSG_SNAPSHOT		54 /// Retrieve termination result



#define MAX_OUTGOING_MSG	50

//#define INGOING_BUFFER_INITIAL_SIZE ((unsigned)(1<<20)) //1MB

#define INGOING_BUFFER_INITIAL_SIZE ((unsigned) ((1<<20)	/ (1024))) // TEST REALLOC

#define NO_MEM 0
#define MEM_ASSIGNED 1

#define INGOING_BUFFER_GROW_FACTOR (2)

#define IN_USE_FLAG ((unsigned) (0x80000000))

#define MARK_AS_IN_USE(SIZE) ((SIZE) | (IN_USE_FLAG))

#define MARK_AS_NOT_IN_USE(SIZE) ((SIZE) & (~IN_USE_FLAG))

#define IS_IN_USE(SIZE) ( ( (SIZE) & (IN_USE_FLAG) ) != 0 )

//HEADER FOTTER & 2 OFFSET. 
#define MIN_BLOCK_DIMENSION ((4)*(sizeof(unsigned)))


//QUESTO "RESTITUISCE" UN UNSIGNED (L'HEADER APPUNTO)
#define HEADER_OF(OFFSET,LID) (*((unsigned*) (((LPS[LID]->in_buffer.base[0])+(OFFSET)) )))

//RESTITUISCE L'INDIRIZZO DELL'HEADER
#define HEADER_ADDRESS_OF(OFFSET,LID) ((unsigned*) (((LPS[LID]->in_buffer.base[0])+(OFFSET)) ))

//RESTITUISCE L'INDIRIZZO DEL FOOTER
#define FOOTER_ADDRESS_OF(OFFSET,SIZE,LID) ((unsigned*) (( (LPS[LID]->in_buffer.base[0]) + (OFFSET) + (sizeof(unsigned)) + (SIZE) ) ))

//QUESTO "RESTITUISCE" un indirizzo
#define PAYLOAD_OF(OFFSET,LID) ((LPS[LID]->in_buffer.base[0])+(OFFSET)+(sizeof(unsigned)))

//RICORDATI CHE LA DIMENSIONE È NELL'HEADER E CHE È GIA AL NETTO DI HEADER E FOOTER
#define FREE_SIZE(OFFSET,LID) (HEADER_OF(OFFSET,LID))

//occhio che questo "ritorna" l'offset del successivo al blocco che ha header in offset non l'indirizzo
#define NEXT_FREE_BLOCK(OFFSET,LID) (*((unsigned*)((LPS[LID]->in_buffer.base[0]) + (OFFSET) + (2*sizeof(unsigned)))))

//INDIRIZO IN CUI È SCRITTO IL NEXT_FREE
#define NEXT_FREE_BLOCK_ADDRESS(OFFSET,LID) ((unsigned*)((LPS[LID]->in_buffer.base[0]) + (OFFSET) + (2*sizeof(unsigned))))

//occhio che questo "ritorna" l'offset del precedente al blocco che ha header in offset non l'indirizzo
#define PREV_FREE_BLOCK(OFFSET,LID) (*((unsigned*)((LPS[LID]->in_buffer.base[0]) + (OFFSET) + (sizeof(unsigned)))))

//INDIRIZO IN CUI È SCRITTO IL PREV_FREE
#define PREV_FREE_BLOCK_ADDRESS(OFFSET,LID) ((unsigned*)(((LPS[LID]->in_buffer.base[0]) + (OFFSET) + (sizeof(unsigned)))))

#define IS_NOT_AVAILABLE(OFFSET,LID) ( ((OFFSET) == (IN_USE_FLAG)) || (IS_IN_USE(HEADER_OF(OFFSET,LID))))	

#define IS_AVAILABLE(OFFSET,LID) (!(IS_NOT_AVAILABLE(OFFSET,LID)))


//L'INDICAZIONE SE È OCCUPATO O MENO È NELL'HEADER E NEL FOOTER
typedef struct _ingoing_buffer{
	void*	 base[2];
	//first_free sarà offset in quanto può essere tutto spostato con realloc
	unsigned first_free;
	unsigned size[2];
	spinlock_t lock[2];
}ingoing_buffer;


/// This structure is used by the communication subsystem to handle outgoing messages
typedef struct _outgoing_t {
	msg_t outgoing_msgs[MAX_OUTGOING_MSG];
	unsigned int size;
	simtime_t *min_in_transit;
} outgoing_t;


/** @todo Document this structure */
typedef struct _window {
	int msg_number;
	simtime_t min_timestamp;
} window;


/** @todo Document this structure */
typedef struct _wnd_buffer {
	unsigned int first_wnd;
	unsigned int next_wnd;
	window wnd_buffer[WND_BUFFER_LENGTH];
} wnd_buffer;



extern void ParallelScheduleNewEvent(unsigned int, simtime_t, unsigned int, void *, unsigned int);


unsigned alloca_memoria_ingoing_buffer(unsigned , unsigned);
void dealloca_memoria_ingoing_buffer(unsigned, unsigned);
unsigned richiedi_altra_memoria(unsigned lid);
unsigned split(unsigned addr, unsigned size, unsigned lid);
void coalesce(unsigned,unsigned,unsigned,unsigned);
void delete_from_free_list(unsigned, unsigned);
int buffer_switch(unsigned lid);

/* Functions invoked by other modules */
extern void communication_init(void);
extern void communication_fini(void);
extern int comm_finalize(void);
extern void Send(msg_t *msg);
extern simtime_t receive_time_barrier(simtime_t max);
extern int messages_checking(void);
extern void insert_outgoing_msg(msg_t *msg);
extern void send_outgoing_msgs(unsigned int);
extern void send_antimessages(unsigned int, simtime_t);

/* In window.c */
extern void windows_init(void);
extern void register_msg(msg_t *msg);
extern void receive_ack(void);
extern void send_forced_ack(void);
extern simtime_t local_min_timestamp(void);
extern void start_ack_timer(void);




#endif
