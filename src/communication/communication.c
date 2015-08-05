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
* @file communication.c
* @brief This module implements all the communication routines, for exchanging
*        messages among different logical processes and simulator instances.
* @author Francesco Quaglia
* @author Roberto Vitali
*
*/

#include <stdlib.h>
#include <float.h>

#include <core/core.h>
#include <gvt/gvt.h>
#include <queues/queues.h>
#include <communication/communication.h>
#include <statistics/statistics.h>
#include <scheduler/scheduler.h>
#include <scheduler/process.h>
#include <datatypes/list.h>
#include <mm/dymelor.h>
#include <mm/allocator.h> //per allocate_segment

/// This is the function pointer to correctly set ScheduleNewEvent API version, depending if we're running serially or parallelly
void (* ScheduleNewEvent)(unsigned int gid_receiver, simtime_t timestamp, unsigned int event_type, void *event_content, unsigned int event_size);

/// Buffer used by MPI for outgoing messages
//static char buff[SLOTS * sizeof(msg_t)];


/**
* This function initializes the communication subsystem
*
* @author Roberto Vitali
*/
void communication_init(void) {
//	windows_init();
	int i;
	for(i=0;i<n_prc;i++){
		LPS[i]->in_buffer.base = pool_get_memory(LPS[i]->lid, INGOING_BUFFER_INITIAL_SIZE);
		//offset 0
		LPS[i]->in_buffer.first_free = 0;
		//primo header, ricorda che le dimensioni sono già al netto di header e footer
		*(unsigned*)(LPS[i]->in_buffer.base) = INGOING_BUFFER_INITIAL_SIZE - 2 * sizeof(unsigned int);
		//primo footer
		*(unsigned*)((LPS[i]->in_buffer.base) + INGOING_BUFFER_INITIAL_SIZE - sizeof(unsigned int)) = INGOING_BUFFER_INITIAL_SIZE - 2 * sizeof(unsigned int);
		LPS[i]->in_buffer.size = INGOING_BUFFER_INITIAL_SIZE;
	}
}


/**
* This function finalizes the communication subsystem
*
* @author Roberto Vitali
*
*/
void communication_fini(void) {
}





/**
* This function is invoked by the application level software to inject new events into the simulation
*
* @author Francesco Quaglia
*
* @param gid_receiver Global id of logical process at which the message must be delivered
* @param timestamp Logical Virtual Time associated with the event enveloped into the message
* @param event_type Type of the event
* @param event_content Payload of the event
* @param event_size Size of event's payload
*/
void ParallelScheduleNewEvent(unsigned int gid_receiver, simtime_t timestamp, unsigned int event_type, void *event_content, unsigned int event_size) {
	msg_t event;

	switch_to_platform_mode();

	// In Silent execution, we do not send again already sent messages
	if(LPS[current_lp]->state == LP_STATE_SILENT_EXEC) {
		return;
	}

	// Check whether the destination LP is out of range
	if(gid_receiver > n_prc_tot - 1) {	// It's unsigned, so no need to check whether it's < 0
		rootsim_error(false, "Warning: the destination LP %d is out of range. The event has been ignored\n", gid_receiver);
		goto out;
	}

	// Check if the associated timestamp is negative
	if(timestamp < lvt(current_lp)) {
		rootsim_error(true, "LP %d is trying to generate an event (type %d) to %d in the past! (Current LVT = %f, generated event's timestamp = %f) Aborting...\n", current_lp, event_type, gid_receiver, lvt(current_lp), timestamp);
	}

	// Copy all the information into the event structure
	bzero(&event, sizeof(msg_t));
	event.sender = LidToGid(current_lp);
	event.receiver = gid_receiver;
	event.type = event_type;
	event.timestamp = timestamp;
	event.send_time = lvt(current_lp);
	event.message_kind = positive;
	event.mark = generate_mark(current_lp);
	event.size = event_size;

	if(event.type == RENDEZVOUS_START) {
		event.rendezvous_mark = current_evt->rendezvous_mark;
	}

	if (event_content != NULL && event_size>0) {
		spin_lock(&LPS[event.receiver]->in_buffer.lock);
		event.payload_offset = alloca_memoria_ingoing_buffer(event.receiver, event_size);
		memcpy((LPS[event.receiver]->in_buffer.base) + event.payload_offset, event_content, event_size);
		spin_unlock(&LPS[event.receiver]->in_buffer.lock);

	}

	insert_outgoing_msg(&event);

    out:
	switch_to_application_mode();
}



/**
* This function send all the antimessages for a certain LP.
* After the antimessage is sent, the header is removed from the output queue!
*
* @author Francesco Quaglia
*
* @param lid The Logical Process Id
*/
void send_antimessages(unsigned int lid, simtime_t after_simtime) {
	msg_t *anti_msg,
		  *anti_msg_next;

	msg_t msg;

	if (list_empty(LPS[lid]->queue_out))
		return;

	// Get the first message header with a timestamp <= after_simtime
	anti_msg = list_tail(LPS[lid]->queue_out);
	while(anti_msg != NULL && anti_msg->send_time > after_simtime)
		anti_msg = list_prev(anti_msg);

	// The next event is the first event with a sendtime > after_simtime, if any
	if(anti_msg == NULL)
		return;

	anti_msg = list_next(anti_msg);

	// Now send all antimessages
	while(anti_msg != NULL) {
		bzero(&msg, sizeof(msg_t));
		msg.sender = anti_msg->sender;
		msg.receiver = anti_msg->receiver;
		msg.timestamp = anti_msg->timestamp;
		msg.send_time = anti_msg->send_time;
		msg.mark = anti_msg->mark;
		msg.message_kind = negative;

		Send(&msg);

		// Remove the already sent antimessage from output queue
		anti_msg_next = list_next(anti_msg);
		list_delete_by_content(lid, LPS[lid]->queue_out, anti_msg);
		anti_msg = anti_msg_next;
	}
}





/**
*
*
* @author Roberto Vitali
*/
int comm_finalize(void) {

	register unsigned int i;

	// Release as well memory used for remaining input/output queues
	for(i = 0; i < n_prc; i++) {
		while(!list_empty(LPS[i]->queue_in)) {
			list_pop(i, LPS[i]->queue_in);
		}
		while(!list_empty(LPS[i]->queue_out)) {
			list_pop(i, LPS[i]->queue_out);
		}
	}

//	return MPI_Finalize();
	return 0;

}



/**
* Send a message. If it's scheduled to a local LP, update its queue, otherwise
* ask MPI to deliver it to the hosting kernel instance.
*
* @author Francesco Quaglia
*/
void Send(msg_t *msg) {
	// Check whether the message recepient is local or remote
	if(GidToKernel(msg->receiver) == kid) { // is local
		insert_bottom_half(msg);
	} else { // is remote
		rootsim_error(true, "Calling an operation not yet reimplemented, this should never happen!\n", __FILE__, __LINE__);
	}
}



/**
* This function allows kernels to receive time barrier from other instances and calculate
* the maximum value.
*
* @author Francesco Quaglia
*/
/*simtime_t receive_time_barrier(simtime_t max) {
	register unsigned int i;
	simtime_t tmp;

	// Compute the maximum time barrier
	for (i = 0; i < n_ker; i++) {
		if  (i != kid) {
			(void)comm_recv((char*)&tmp, sizeof(simtime_t), MPI_CHAR, (int)i, MSG_TIME_BARRIER, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			if (D_DIFFER(max, -1) || ((tmp - 1) > DBL_EPSILON && tmp > max))
				max = tmp;
		}
	}

	return max;
}*/




#if 0
/**
*
*
* @author Francesco Quaglia
*/
static bool pending_msg(void) {

    int flag = 0;

    (void)comm_probe(MPI_ANY_SOURCE, MSG_EVENT, MPI_COMM_WORLD, &flag, MPI_STATUS_IGNORE);
    return (bool)flag;

}


/**
*
*
* @author Francesco Quaglia
*/
int messages_checking(void) {
	msg_t msg;
	int at_least_one_message = 0;


	// Are we in GVT computation phase?
	// TODO a cosa serve questa parte qui sotto?!
//	if (ComputingGvt() == false)
//		send_forced_ack();


	// Check whether we have received any ack message
	receive_ack();

	// If we have pending messages, we process all of them in order to update the queues
 	while(pending_msg()){

		at_least_one_message = 1;

		// Receive the message
	    	(void)comm_recv((char*)&msg, sizeof(msg_t), MPI_CHAR, MPI_ANY_SOURCE, MSG_EVENT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

		// For GVT Operations
		gvt_messages_rcvd[GidToKernel(msg.sender)] += 1;


		// Update the queues
//		DataUpdating(&msg);

	}

	// Did we receive at least one message?
	return at_least_one_message;

}
#endif





/**
*
*
* @author Francesco Quaglia
*/
void insert_outgoing_msg(msg_t *msg) {

	// Sanity check
	if(LPS[current_lp]->outgoing_buffer.size == MAX_OUTGOING_MSG){
		rootsim_error(true, "Outgoing message queue full! Too many events scheduled by one single event. Aborting...");
	}

	// Message structure was declared on stack in ScheduleNewEvent: make a copy!
	LPS[current_lp]->outgoing_buffer.outgoing_msgs[LPS[current_lp]->outgoing_buffer.size++] = *msg;

	// Store the minimum timestamp of outgoing messages
	if(msg->timestamp < LPS[current_lp]->outgoing_buffer.min_in_transit[LPS[current_lp]->worker_thread]) {
		LPS[current_lp]->outgoing_buffer.min_in_transit[LPS[current_lp]->worker_thread] = msg->timestamp;
	}
}



void send_outgoing_msgs(unsigned int lid) {

	register unsigned int i = 0;
	msg_t *msg;

	for(i = 0; i < LPS[lid]->outgoing_buffer.size; i++) {
		msg = &LPS[lid]->outgoing_buffer.outgoing_msgs[i];
		Send(msg);

		// Register the message in the sender's output queue, for antimessage management
		(void)list_insert(msg->sender, LPS[msg->sender]->queue_out, send_time, msg);
	}

	LPS[lid]->outgoing_buffer.size = 0;
}

void richiedi_altra_memoria(unsigned lid){
//	spin_lock(&LPS[lid]->in_buffer.lock);
	int new_size = (LPS[lid]->in_buffer.size) * INGOING_BUFFER_GROW_FACTOR;
	LPS[lid]->in_buffer.base = pool_realloc_memory(LPS[lid]->lid, new_size, LPS[lid]->in_buffer.base);
	LPS[lid]->in_buffer.size = new_size;
//	spin_unlock(&LPS[lid]->in_buffer.lock);
}

int alloca_memoria_ingoing_buffer(unsigned int lid, int size){
	//il chiamante si deve preoccupare di fare lo spinlock
	int ptr_offset;
	ptr_offset = assegna_blocco(lid,size);
	return ptr_offset;
}

//L'OPERAZIONE DI SPLIT E' UN'OPERAZIONE CHE DIVIDE UN BLOCCO DI MEMORIA E CAMBIA GLI HEADER AD ENTRAMBI
//param addr è l'offset relativo all'header del blocco che sto allocando
//param size è la size che mi serve in quel blocco...
//tutti i controlli che sia il blocco giusto sono fatti altrove!
//return 1 se il blocco che si è creato è di dimensione >= a quella minima
//return 0 se il blocco che si è creato è troppo piccolo
int split(unsigned addr, int* size, int lid){
		
	unsigned splitted = addr + 2 * sizeof(unsigned) + (*size);
	unsigned addr_size = FREE_SIZE(addr,lid);
	int splitted_size;
	int ret = 0;
	unsigned size_with_flag;
	
	//se il blocco successivo non è in uso
	if( (HEADER_OF(splitted,lid) & IN_USE_FLAG) == 0){
	
		splitted_size = addr_size-(*size);
		//- h e f successivi;
		splitted_size -= 2*sizeof(unsigned);
		
		//spazio minimo per essere un blocco libero che contiene il "puntatore" al prossimo libero
		splitted_size -= sizeof(unsigned);
		
//		printf("%d\n", splitted_size);
		
		//se è troppo piccolo lo alloco tutto.. piccola frammentazione interna inevitabile
		if(splitted_size<0){
			*size = addr_size;
//			printf("size=%d\n", *size);
		}

		
		//se il blocco successivo non è fuori dai limiti 
		else if(splitted < LPS[lid]->in_buffer.size){
				
			//ok è della dimensione giusta, ci rimetto lo spazio libero (quello che eventualmente serve come "puntatore")
			splitted_size+=sizeof(unsigned);
			
			//se splitted sarà in grado di contenere h+f+puntatore al successivo libero
			ret = 1;
				
			//metto l'header al nuovo blocco
			memcpy(LPS[lid]->in_buffer.base + splitted, &splitted_size, sizeof(unsigned));
						
			//metto il footer al nuovo blocco che si trova a splitted+new_dim+header
			memcpy(LPS[lid]->in_buffer.base + splitted + splitted_size + sizeof(unsigned), &splitted_size, sizeof(unsigned));
		}
	}
	size_with_flag = (*size) | IN_USE_FLAG;
	
	//DEVO AGGIORNARE L'HEADER E IL FOOTER DEL BLOCCO CHE HO APPENA ALLOCATO. RICORDA ANCHE L'OR CON IN USE
	memcpy(LPS[lid]->in_buffer.base + addr, &size_with_flag, sizeof(unsigned));
	memcpy(LPS[lid]->in_buffer.base + addr + sizeof(unsigned) + (*size), &size_with_flag, sizeof(unsigned));
	
	return ret;
}

//ricordati che questo restituisce già l'offset per il payload! 
int assegna_blocco(unsigned int lid, int size){
	//offset del successivo libero
	unsigned succ;
	unsigned add_to_ret;
	int res;
	unsigned actual_free_space;
	unsigned actual = LPS[lid]->in_buffer.first_free;
	//solo quella nuova	
	unsigned new_memory_size;
	//non c'è memoria libera
//	puts("in");

	if(actual == -1){
		puts("eccomi");
		richiedi_altra_memoria(lid);
		//first_free diventa il primo byte appena allocato
		new_memory_size = LPS[lid]->in_buffer.size / INGOING_BUFFER_GROW_FACTOR;
		LPS[lid]->in_buffer.first_free = new_memory_size;
		//lo inizializzo come un unico buffer
		//header...
		memcpy(LPS[lid]->in_buffer.base + new_memory_size, &new_memory_size, sizeof(unsigned));
		
		//...footer
		memcpy((2* new_memory_size) - sizeof(unsigned) + LPS[lid]->in_buffer.base, &new_memory_size, sizeof(unsigned));
		actual = LPS[lid]->in_buffer.first_free;
	}
	
	//a questo punto first_free è sicuro un blocco libero
	actual_free_space = FREE_SIZE(actual,lid);
	
	//questo è il caso in cui è proprio il primo ad essere quello buono
	//le dimensioni sono già al netto di header e footer
	if(actual_free_space >= size){
	
		res = split(actual,&size,lid);
		
		if(res == 1){
				//se dopo c'è spazio libero a sufficienza nel nuovo blocco che si è creato
				//a seguito dello split
				//avanzo il first free
				LPS[lid]->in_buffer.first_free = actual + size + 2*sizeof(unsigned);
//				puts("1");
		}
		//altrimenti provo a vedere se il free a cui lui puntava è effettivamente ancora libero
		//questo torna utile soprattutto all'inizio quando la memoria è tutta azzerata
		//lui punta a 0 ma 0 può essere stato allocato
		else if ((NEXT_FREE_BLOCK(actual, lid)) & IN_USE_FLAG ==0){
//			puts("2");
			LPS[lid]->in_buffer.first_free = NEXT_FREE_BLOCK(actual, lid);
		}
		//altrimenti provo a vedere se dopo di lui c'è un blocco libero che non è fuori dai limiti
		//(questo andrà eliminato quando implementerò il coalescing)
		else if ( (actual + 2 * sizeof(unsigned) + size < LPS[lid]->in_buffer.size) && ((actual + 2 * sizeof(unsigned) + size) & IN_USE_FLAG == 0)){
//			puts("3");
			LPS[lid]->in_buffer.first_free = actual + 2 * sizeof(unsigned) + size;
		}
		//a sto punto amen
		else{
//			puts("4");
			//non c'è più spazio libero
			LPS[lid]->in_buffer.first_free = -1;
		}
		add_to_ret = actual;
		goto esci;
	}
	
	//questo è il caso in cui non è il primo ad essere quello buono
	//a questo punto si accorge che non ce la fa con il primo blocco libero e cerca una soluzione.
	//finche non implementiamo la free l'unica soluzione possibile è chiedere altra memoria!
	//tieni presente questa cosa!!
	int i=0;
	while(true){
		
//		printf("----%d\n", i++);
		succ = NEXT_FREE_BLOCK(actual,lid);

		//a questo punto dobbiamo capire qual'è il successivo libero. Se siamo nella fase iniziale
		//qua mi dice che il successivo libero è 0. questa Cosa può essere vera o meno a secondo
		//dell'esecuzione. quindi controlliamo se c'è il flag IN_USE. Non può accadere che sia
		//buono quello successivo e che il blocco "non lo sappia". In quel caso siamo già a regime
		//e il blocco deve saperlo. L'unico caso in cui il blocco non sa qual'è il successivo libero è 
		//al primo giro		
		if( (HEADER_OF(succ,lid) & IN_USE_FLAG) != 0) {
//			puts("entro qua");
			richiedi_altra_memoria(lid);
			//new_memory_size è la memoria appena allocata che corrisponde anche all'offset del primo blocco libero
			new_memory_size = LPS[lid]->in_buffer.size / INGOING_BUFFER_GROW_FACTOR;
//			printf("new:memory_size = %d", new_memory_size);
			//concateno ad actual il nuovo blocco
			memcpy(PAYLOAD_OF(actual,lid), &new_memory_size, sizeof(unsigned));
			
			//new_memory size = old_memory size che è uguale allo spiazzamento (fino a -) è occupato)
			succ = new_memory_size;
			
			int new_memory_size_netta = new_memory_size - 2 * sizeof(unsigned);
			//header... ricordandoci di levare lo spazio per header e footer
			memcpy(succ + LPS[lid]->in_buffer.base, &new_memory_size_netta, sizeof(unsigned));
			//...footer
			memcpy(PAYLOAD_OF(succ,lid) + new_memory_size_netta, &new_memory_size_netta, sizeof(unsigned));
		}
//		puts("if superato");
		
		//ora ho sicuramente un successivo
		int succ_size = FREE_SIZE(succ,lid);
//		printf("succ_size = %u\n", succ_size);
		//e se il successivo ha la dimensione che mi serve
		if(succ_size > size){
//			puts("--------------------");
			//è il blocco che cerco
			add_to_ret = succ;
			succ += 2*sizeof(unsigned)+size; //mi serve per dopo se il blocco che si è creato non basta
			res = split(add_to_ret, &size, lid);
			//cambio il successivo ad actual
			if(res==0)
				//se il blocco che si è creato non basta
				memcpy(PAYLOAD_OF(actual,lid), PAYLOAD_OF(add_to_ret,lid), sizeof(unsigned));
			else
				//altrimenti se basta
				memcpy(PAYLOAD_OF(actual,lid), &succ, sizeof(unsigned));
			goto esci;
		}
		else{
			//se non basta passo appresso
			actual = succ;
		}
	}
	
	esci:
	
	return add_to_ret;
}



/*DA ELIMINARE
int alloca_memoria_ingoing_buffer(unsigned int lid, int size){
	int ptr_offset;
	if((LPS[lid]->in_buffer.size - LPS[lid]->in_buffer.offset) < size){
		richiedi_altra_memoria(lid);
	}
	ptr_offset = LPS[lid]->in_buffer.offset;
	LPS[lid]->in_buffer.offset+=size;
	return ptr_offset;
	
}
*/


void dealloca_memoria_ingoing_buffer(unsigned int lid, void* ptr, int size){
	//probabilmente anche questo dovrà essere synch
	//puts("not yet implemented");
}
