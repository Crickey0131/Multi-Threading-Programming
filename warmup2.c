#include <pthread.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "packet.h"
#include <unistd.h>
#include <sys/time.h>
#include "my402list.h"
#include "cs402.h"
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>

//threads
pthread_t pathread;//packetarrival thread
pthread_t tdthread;//tokendeposit thread
pthread_t s1thread;//server1 thread
pthread_t s2thread;//server2 thread
pthread_t scthread;//signal catching thread

//functions
//first proceudres
void *arrival(void *arg);
void *deposit(void *arg);
void *server1(void *arg);
void *server2(void *arg);
void *sigcatch(void *arg);

//get the current time
int ptime();
//covert the time into ms
double toms(int time);
//parse the command for initialization
FILE *parsecmd(int argc, char *argv[]);
//initialize the comming packet
int initpck(FILE* fp, packet *pp);
//cleanup routines, called after pathread is cancelled
void pacleanup(void *arg);

int is_dir(char *path);

//mutexed parameters: token_num, Q1, Q2, packet_num
My402List Q1;
My402List Q2;
int token_num = 0;


char buf[1024];


//Running Parameters
double lambda = 1;  //packets per second (interarrival)
double mu = 0.35;   //packets per second (service)
double r = 1.5;     //tokens  per second
int capacity = 10;//B
int p = 3;  
int packet_count = 20;//num


int packet_num = 0;
int last_arrive_time;
int emu_start;
struct timeval mtime;
FILE *fp;
int trace = 0;//trace mode

//int serverqt = 0;//whether server thread can exit

int cleanup = 0;//if the cleanup already done

int paexit = 0; //if pathread already exit

char* filename;

//<Ctrl-C>
sigset_t set;



//for statistics
int token_serial = 0;
double inter_arrival_sum = 0;
double service_sum = 0;
double time_Q1 = 0;
double time_Q2 = 0;
double time_S1 = 0;
double time_S2 = 0;
double sys_sum = 0;
double sys_sqr_sum = 0;
int com_pck = 0;
int token_drop = 0;
int packet_drop = 0;


pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv = PTHREAD_COND_INITIALIZER;//not_empty_queue


int main(int argc, char *argv[]) {
    
    errno = 0;

    //<Ctrl-C> related stuff
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, 0);//blocked SIGINT for all threads

    //determing which mode
    if((fp = parsecmd(argc, argv)) != NULL) {
	trace = 1;
    }
    printf("mode is %d\n", trace);

    //Initialize Q1 and Q2
    memset(&Q1, 0, sizeof(My402List));
    (void)My402ListInit(&Q1);    //empty, just an anchor
    memset(&Q2, 0, sizeof(My402List));
    (void)My402ListInit(&Q2);    //empty, just an anchor



    printf("Emulation Parameters\n"); 

    printf("    number to arrive = %d\n", packet_count);//must 
    
    if(trace == 0) {
        printf("    lambda = %g\n", lambda);
        printf("    mu = %g\n", mu);
    }
    
    printf("    r = %g\n", r);//must
    printf("    B = %d\n", capacity);//must
    
    if(trace == 0) {
	   printf("    P = %d\n", p);   
    }
    //maybe in the function
    if(trace == 1) {
	   printf("    tsfile = %s\n\n", filename);   
    }
    


    printf("%012.3lfms: emulation begins\n", 0.0); 
    emu_start = ptime();
    //emulation started
    last_arrive_time = emu_start;  

    //return code // argument
    pthread_create(&pathread, 0, arrival, 0);
    pthread_create(&tdthread, 0, deposit, 0);
    pthread_create(&s1thread, 0, server1, 0);
    pthread_create(&s2thread, 0, server2, 0);
    pthread_create(&scthread, 0, sigcatch, 0);

    //terminate threads
    pthread_join(pathread, 0);
    pthread_join(tdthread, 0);
    pthread_join(s1thread, 0);
    pthread_join(s2thread, 0);

    double total_time = toms(ptime() - emu_start);
    printf("%012.3lfms: emulation ends\n\n\n", total_time); 


    //Statistics
    printf("Statistics:\n\n");

    printf("average packet inter-arrival time = %.6g seconds\n", (inter_arrival_sum*0.001)/packet_count);

    if(com_pck == 0) {
	printf("average packet service time inavailable: No completed packets!\n");
    }
    else {
        printf("average packet service time = %.6g seconds\n", (service_sum*0.001)/com_pck);
    }

    printf("average number of packets in Q1 = %.6g\n", time_Q1/total_time);

    printf("average number of packets in Q2 = %.6g\n", time_Q2/total_time);

    printf("average number of packets in S1 = %.6g\n", time_S1/total_time);

    printf("average number of packets in S2 = %.6g\n\n", time_S2/total_time);

    if(com_pck == 0) {
        printf("average time a packet spent in system inavailable: No completed packets!\n");
        printf("standard deviation for time spent in system inavailable: No completed packets!\n");
    }
    else {
        //E(x) in the unit of second
        double e2 = (sys_sum*0.001)/com_pck;
        printf("average time a packet spent in system = %.6gs\n", (sys_sum*0.001)/com_pck);

        //standard deviation
        //E(x^2), get the square sum
        double e1 = (sys_sqr_sum*0.000001)/com_pck;

        double var = e1 - e2*e2;
        double sd = sqrt(var);
        printf("standard deviation for time spent in system =  %.6g\n", sd);

    }

    if(token_serial == 0) {
        printf("token drop probability inavailable: Tokens haven't arrived!\n");
    }
    else {
        printf("token drop probability =  %.6g\n", token_drop/(double)token_serial);
    }

    if(packet_num == 0) {
        printf("packet drop probability inavailable: Packets haven't arrived!\n\n");
    }
    else {
        printf("packet drop probability =  %.6g\n", packet_drop/(double)packet_num);
    }

    return 0;
}




//my dear first procedures, no bugs please...
//1 packet arrival thread
void *arrival(void *arg) {
    
    /*
    int bkkp1 = 0;
    int bkkp2 = 0;
    int bkkp3 = 0;
    int bkkp4 = 0;
*/
    pthread_cleanup_push(pacleanup, arg);

    for(;;) {
        
        //bkkp1 = ptime();

	//create and initialize the packet object
	packet *newp;
        if((newp = malloc(sizeof(packet))) == NULL) {
	    (void)fprintf(stderr, "ERROR: packet malloc failed");
	    exit(0);
        }

	//deterministic mode
	if(trace == 0) {
        
        
        if(1/lambda > 10) {
            newp->inter_arrival_time = 10000000;
        }
        else {
            newp->inter_arrival_time = 1000000/lambda;
        } 
	    //printf("the inter_arrival_time is %d\n", newp->inter_arrival_time);
        
        if(1/mu > 10) {
            newp->service_time = 10000000;
        }
        else {
            newp->service_time = 1000000/mu;
        }
	    //printf("the service_time is %d\n", newp->service_time);
	    newp->request = p;
	}
	else {//trace-driven mode
	//return 0 if success, return 1 if fail
	    initpck(fp, newp);
	}	
        
        //bkkp2 = ptime();
        
    //int d = (bkkp2 - bkkp1) + (bkkp4 - bkkp3);
//\\\\\\\\\\\\\\\\\\\\___________________
	//sleep for an interval
	usleep(newp->inter_arrival_time);//try to deduct the bookkeeping time
        //usleep(1);
        
//\\\\\\\\\\\\\\\\\\\_____________________

        //bkkp3 = ptime();
	//event: p1 arrives
	newp->arrive_time = ptime();




	pthread_mutex_lock (&mutex);




	//get the packet_num before enqueues the packet to Q1
	packet_num++;
	newp->num = packet_num;
	

	//output1: actual inter-arrival time
	printf("%012.3lfms: p%d arrives, needs %d tokens, inter-arrival time = %.3lfms", toms(newp->arrive_time - emu_start), packet_num, newp->request, toms(newp->arrive_time - last_arrive_time));

	inter_arrival_sum += toms(newp->arrive_time - last_arrive_time);////////////////////////////////

	last_arrive_time = newp->arrive_time;

	//request > depth, drop the packet
	if(newp->request > capacity) {
	    printf(", dropped\n");
	    packet_drop++;

	    if(packet_num == packet_count) {
	        printf("pa: no more packets, pthread_exit\n");
	        pthread_mutex_unlock (&mutex);
		//broadcast here
            pthread_cond_broadcast(&cv);
	        pthread_exit((void *)1);
	    }

	    pthread_mutex_unlock (&mutex);
	    continue;
	} 
	else {
	    printf("\n");
	}


	//event: p1 enters Q1
	My402ListAppend(&Q1, newp);
	newp->Q1_start = ptime();
	//output2
	printf("%012.3lfms: p%d enters Q1\n", toms(newp->Q1_start - emu_start), packet_num);

	//if it is the first packet, try to dequeue Q1
	if (My402ListLength(&Q1) == 1) {
	    //move the first packet from Q1 to Q2 if there are enough tokens
	    if(token_num >= newp->request) {
            //code modifying the guard
            //printf("pa: dequeue Q1\n");	

            token_num-=newp->request;

            //Q1--; get the Q1_start in the middle
            My402ListElem *ep = My402ListFirst(&Q1);
            packet *pp = (packet *)ep->obj;


            //event: p1 leaves Q1
            My402ListUnlink(&Q1, ep);

            //output3
            int curr_time = ptime();
            printf("%012.3lfms: p%d leaves Q1, time in Q1 = %.3lfms, token bucket now has %d token\n", toms(curr_time - emu_start), pp->num, toms(curr_time - pp->Q1_start), token_num);

            time_Q1 += toms(curr_time - emu_start);


            if(My402ListEmpty(&Q2)) {//server must be sleeping
                //event: p1 enters Q2
                My402ListAppend(&Q2, pp);
                pp->Q2_start = ptime();
                //output4
                printf("%012.3lfms: p%d enters Q2\n", toms(pp->Q2_start - emu_start), pp->num);

                //signal
                pthread_cond_broadcast(&cv);
            }
            else {
                //event: p1 enters Q2
                My402ListAppend(&Q2, pp);
                pp->Q2_start = ptime();
                //output4
                printf("%012.3lfms: p%d enters Q2\n", toms(pp->Q2_start - emu_start), pp->num);
            }
        }
        
	    //not enough tokens, no busywait
	    else {
		//usleep(token generate time)
		//usleep(100000);
		//printf("pa: not enough tokens\n");
	    }
        
	}//My402ListLength(&Q1) == 1

    
    //else it is not the first packet


	//when to stop
	if(packet_num == packet_count) {
	    printf("pa: no more packets, pthread_exit\n");
        paexit = 1;
	    pthread_mutex_unlock (&mutex);
	    pthread_exit((void *)1);
	}



	pthread_mutex_unlock (&mutex);
	//go back to sleep for a right amount

        //bkkp4 = ptime();
    }//for

    pthread_cleanup_pop(0);

    return (0);
}

void pacleanup(void *arg) {

    pthread_mutex_lock (&mutex);
    printf("callning pacleanup successfully!\n");

    
    //if pathread not dead, or dead but Q1, Q2 not empty), do the cleanup
    if(paexit != 1) {


        while(!My402ListEmpty(&Q1)) {

            My402ListElem* ep = My402ListFirst(&Q1);
            packet *pp = (packet *)ep->obj;
            printf("%012.3lfms: p%d removed from Q1\n", toms(ptime()-emu_start), pp->num);
            My402ListUnlink(&Q1, ep);
        }
        
        while(!My402ListEmpty(&Q2)) {

            My402ListElem* ep = My402ListFirst(&Q2);
            packet *pp = (packet *)ep->obj;
            //printf("p%d removed from Q2\n", pp->num);
            printf("%012.3lfms: p%d removed from Q2\n", toms(ptime()-emu_start), pp->num);
            My402ListUnlink(&Q2, ep);
        }
        
        //I've done the cleanup, now the server can quit
        //serverqt = 1;
        
        //cleanup done now
        cleanup = 1;

    }
    

    pthread_mutex_unlock (&mutex);


    //broadcast to servers so that they can quit
    pthread_cond_broadcast(&cv);

}



//2 token depositing thread
void *deposit(void *arg) {//sits in a loop
    
    //int bkkp1 = 0;
    //int bkkp2 = 0;
    
    for(;;) {
	//sleep for an interval
	//usleep(500000); could it be an double rather than int?
        //int d = bkkp2 - bkkp1;

	if(1/r > 10) {
	    usleep(10000000);
	}
	else {
	    usleep(1000000/r);
	}
	//printf("token sleep for %lf microseconds\n", 1000000/r);

        
    //bkkp1 = ptime();


	pthread_mutex_lock (&mutex);

	//if no overflow, increment token count
	if(token_num < capacity) {
	    token_num++;
	    printf("%012.3lfms: token t%d arrives, token bucket now has %d token\n", toms(ptime() - emu_start), ++token_serial ,token_num);
	}
	else {
        token_drop++;
	    printf("%012.3lfms: token t%d arrives, dropped\n", toms(ptime() - emu_start), ++token_serial);
	    
	}


    //check if it can move first packet from Q1 to Q2
	if(!My402ListEmpty(&Q1)) {
        
        //Q1--; get the Q1_start in the middle
	    My402ListElem *ep = My402ListFirst(&Q1);
	    packet *pp = (packet *)ep->obj;   
        
	    //code modifying the guard
        
        //if can move packet from Q1 to Q2....
        if(token_num >= pp->request) {
            
            token_num-=pp->request;
            
            //event: p1 leaves Q1
	        My402ListUnlink(&Q1, ep);
	    
	        //output3
	        int curr_time = ptime();
	        printf("%012.3lfms: p%d leaves Q1, time in Q1 = %.3lfms, token bucket now has %d token\n", toms(curr_time - emu_start), pp->num, toms(curr_time - pp->Q1_start), token_num);

	        time_Q1 += toms(curr_time - emu_start);

            if(My402ListEmpty(&Q2)) {//server must be sleeping
            //event: p1 enters Q2
            My402ListAppend(&Q2, pp);
            pp->Q2_start = ptime();
            //output4
            printf("%012.3lfms: p%d enters Q2\n", toms(pp->Q2_start - emu_start), pp->num);

            //free(ep);
            //signal
            pthread_cond_broadcast(&cv);
            }
            else {//Q2 is not empty
            //event: p1 enters Q2
            My402ListAppend(&Q2, pp);
            pp->Q2_start = ptime();
            //output4
            printf("%012.3lfms: p%d enters Q2\n", toms(pp->Q2_start - emu_start), pp->num);
		
	        }
            
        }

	}//Q1 is not empty
	else {//Q1 is empty
        //when to stop
        if(packet_num == packet_count) {
            printf("td: number of tokens is %d, pthread_exit\n", token_num);
            pthread_mutex_unlock (&mutex);
            pthread_exit((void *)2);
        }

	}


	pthread_mutex_unlock (&mutex);

        //bkkp2 = ptime();
    }//for
    return (0);
}



//lightning service
//3
void *server1(void *arg) {

    for(;;) {


	pthread_mutex_lock(&mutex);


	//guard not true
	while(My402ListEmpty(&Q2)) {// && !shutdown

	    //no packets
	    if((packet_num == packet_count || cleanup == 1) && My402ListEmpty(&Q1)) {
	        printf("s1: Job is done. pthread_exit\n");
	        pthread_mutex_unlock(&mutex);
	        pthread_exit((void *)3);
	    }

	    //printf("s1: Go to Sleep.\n");
	    pthread_cond_wait(&cv, &mutex);
        
        //do I need to lock here? it did it self
        //pthread_mutex_lock(&mutex);
        /*
        if(cleanup == 1) {
            printf("s1: Job is done. pthread_exit\n");
	        pthread_mutex_unlock(&mutex);
	        pthread_exit((void *)3);
        }
        */

	}
        //the pathread stop and do the cleanup and terminate


	//printf("s1: Wake Up and dequeue Q2\n");
	//dequeue a job atomically
	    
	My402ListElem *ep = My402ListFirst(&Q2);
	packet *pp = (packet *)ep->obj;

	//event: p1 leaves Q2
	My402ListUnlink(&Q2, ep);
	//output5
	int leaves_Q2_time = ptime();
	printf("%012.3lfms: p%d leaves Q2, time in Q2 = %.3lfms\n", toms(leaves_Q2_time - emu_start), pp->num, toms(leaves_Q2_time - pp->Q2_start));
	
	time_Q2 += toms(leaves_Q2_time - pp->Q2_start);

	pthread_mutex_unlock(&mutex);
        
        
        
        
        
	//work on the job based on its service_time

	//event: p1 begins service at S1
	int time_enter_server = ptime();

	//ouput6
	printf("%012.3lfms: p%d begins service at S1, requesting %.3lfms of service\n", toms(time_enter_server - emu_start), pp->num, toms(pp->service_time));

	//serving...
	usleep(pp->service_time);
	//event: p1 departs from S1 (free packet?)
	int time_leave = ptime();


	//output7 departs
	printf("%012.3lfms: p%d departs from S1, service time = %.3lfms, time in system = %.3lfms\n", toms(time_leave - emu_start), pp->num, toms(time_leave - time_enter_server), toms(time_leave - pp->arrive_time));	

	double sys_time = toms(time_leave - pp->arrive_time);

///////////////////////////////////////
	pthread_mutex_lock(&mutex);

	com_pck++;
	service_sum += toms(time_leave - time_enter_server);
	time_S1 += toms(time_leave - time_enter_server);
	sys_sum += toms(time_leave - pp->arrive_time);
	sys_sqr_sum += sys_time * sys_time;

	pthread_mutex_unlock(&mutex);
////////////////////////////////////////
    }//for
    return (0);
}


//4
void *server2(void *arg) {

    for(;;) {




	pthread_mutex_lock(&mutex);

	//guarded command?
	while(My402ListEmpty(&Q2)) {// && !shutdown(serverqt?)

	    if((packet_num == packet_count || cleanup == 1) && My402ListEmpty(&Q1)) {
	        printf("s2: Job is done. pthread_exit\n");
	        pthread_mutex_unlock(&mutex);
	        pthread_exit((void *)4);
	    }


	    //printf("s2: Go to Sleep.\n");
	    pthread_cond_wait(&cv, &mutex);
        
        //do I need to lock here?
        //pthread_mutex_lock(&mutex);
        /*
        if(cleanup == 1) {
            printf("s2: Job is done. pthread_exit\n");
	        pthread_mutex_unlock(&mutex);
	        pthread_exit((void *)4);
        }
        */

	}


	//printf("s2: Wake Up and dequeue Q2\n");
	//dequeue a job atomically
	//Q2--;
	    
	My402ListElem *ep = My402ListFirst(&Q2);
	packet *pp = (packet *)ep->obj;

	//event: p1 leaves Q2
	My402ListUnlink(&Q2, ep);
	//output5
	int leaves_Q2_time = ptime();
	printf("%012.3lfms: p%d leaves Q2, time in Q2 = %.3lfms\n", toms(leaves_Q2_time - emu_start), pp->num, toms(leaves_Q2_time - pp->Q2_start));
	
	time_Q2 += toms(leaves_Q2_time - pp->Q2_start);

	pthread_mutex_unlock(&mutex);





	//event: p1 begins service at S2
	int time_enter_server = ptime();

	//ouput6
	printf("%012.3lfms: p%d begins service at S2, requesting %.3lfms of service\n", toms(time_enter_server - emu_start), pp->num, toms(pp->service_time));



	//serving...
	usleep(pp->service_time);
	//event: p1 departs from S2 (free packet?)
	int time_leave = ptime();

	//output7 departs
	printf("%012.3lfms: p%d departs from S2, service time = %.3lfms, time in system = %.3lfms\n", toms(time_leave - emu_start), pp->num, toms(time_leave - time_enter_server), toms(time_leave - pp->arrive_time));

	double sys_time = toms(time_leave - pp->arrive_time);

/////////////////////////////////////
	pthread_mutex_lock(&mutex);

	com_pck++;
	service_sum += toms(time_leave - time_enter_server);
	time_S2 += toms(time_leave - time_enter_server);
	sys_sum += toms(time_leave - pp->arrive_time);
	sys_sqr_sum += sys_time * sys_time;
	//free the packet?

	pthread_mutex_unlock(&mutex);
///////////////////////////////////////

    }//for
    return (0);
}


//5 signal catching thread xxx
void *sigcatch(void *arg) {
    //int sig;
    while(1) {
	sigwait(&set);
	printf("Got the signal!\n");

	//the terminated thread do the clean up itself
	//pathread not exit
	if(paexit == 0) {
	    pthread_cancel(pathread);
	    printf("pathread cancelled\n");
	}
	//pathread already exit 1.hasn't do the cleanup
	//because cleanup only because of <Ctrl-C>
	//then I will do the cleanup for you
	else {
        paexit = 2;//special case that I do the cleanup for you
	    pacleanup(arg);
	}


	pthread_cancel(tdthread);
	printf("tdthread cancelled\n");
	
    }

}





int ptime() {
    //return code
    gettimeofday(&mtime, 0);
    return mtime.tv_sec * 1000000 + mtime.tv_usec;
    //printf("time is %ld\n", mtime.tv_sec * 1000000 + mtime.tv_usec);
}

double toms(int time) {

    double res = ((double)time)/1000;
    return res;
}


//if file not exist, use the parameters, set packet_count
FILE *parsecmd(int argc, char *argv[]) {
    
    if(argc%2 == 0) {
        fprintf(stderr, "command line syntax should be: warmup2 [-lambda lambda] [-mu mu] [-r r] [-B B] [-P P] [-n num] [-t tsfile]\n");
        exit(1);
    }

    //from the first parameter
    int index = 0;

    FILE *fp = NULL;
    //determine whether it can return a file
    while(++index < argc) {
        
        if(index%2 != 0) {
            if(strcmp(argv[index], "-lambda") != 0 && strcmp(argv[index], "-mu") != 0 && strcmp(argv[index], "-r") != 0 && strcmp(argv[index], "-B") != 0 && strcmp(argv[index], "-P") != 0 && strcmp(argv[index], "-n") != 0 && strcmp(argv[index], "-t") != 0) {
                fprintf(stderr, "command line syntax should be: warmup2 [-lambda lambda] [-mu mu] [-r r] [-B B] [-P P] [-n num] [-t tsfile]\n");
                exit(1);
            }
        }

        
        if(strcmp(argv[index], "-r") == 0) {
            //printf("r is %s\n", argv[++index]);
            //r = atof(argv[++index]);
            double readnum = atof(argv[++index]);
            if(readnum == 0) {
                fprintf(stderr, "command line syntax should be: warmup2 [-lambda lambda] [-mu mu] [-r r] [-B B] [-P P] [-n num] [-t tsfile]\n");
                exit(1);
            }
            if(readnum < 0) {
                printf("-r is wrong, use default value\n");
            }
            else {
                r = readnum;
            }
        }
        
        else if(strcmp(argv[index], "-B") == 0) {
            //printf("B is %s\n", argv[++index]);
            int readnum = atoi(argv[++index]);
            if(readnum == 0) {
                fprintf(stderr, "command line syntax should be: warmup2 [-lambda lambda] [-mu mu] [-r r] [-B B] [-P P] [-n num] [-t tsfile]\n");
                exit(1);
            }
            if(readnum < 0 || readnum > 2147483647) {
                printf("-B is wrong, use default value\n");
            }
            else {
                capacity = readnum;
            }
        }


        else if(strcmp(argv[index], "-t") == 0) {
            //printf("tsfile is %s\n", argv[++index]);
            
            if(is_dir(argv[index+1])) {
                fprintf(stderr, "input file %s is a directory\n", argv[index+1]);
                exit(1);

            }       
            
            fp = fopen(argv[++index], "r");

            filename = argv[index];
            
            	//open failed
            if(fp == NULL) {

                if(errno == 2) {
                    fprintf(stderr, "input file %s does not exist\n", argv[index]);
                }
                else if(errno == 13) {
                    fprintf(stderr, "input file %s can not be opened - access denies\n", argv[index]);
                }
                exit(1);
            }

            else {//open file successfully
                //get the number to come?packet_count = 
                if(fgets(buf, sizeof(buf), fp) == NULL) {
                        fprintf(stderr, "Error: Empty input.\n");
                        exit(1);//clean up?
                }
                else {
                    //printf("the first line of the file is %s\n", buf);
                    if(atoi(buf) == 0) {
                        fprintf(stderr, "input file is not in the right format\n");
                        exit(0);
                    }
                    packet_count = atoi(buf);
                }
                //return fp;
            }
            //break;
        }

    }
    
    if(fp != NULL) {
	   return fp;
    }    

    index = 0;

    printf("argc is %d\n", argc);
    while(++index < argc) {

        if(strcmp(argv[index], "-lambda") == 0) {
            //printf("lambda is %s\n", argv[++index]);
            //lambda = atof(argv[++index]);
            double readnum = atof(argv[++index]);
            //double readnum = strtod(argv[++index], NULL);
            if(readnum == 0) {
                fprintf(stderr, "command line syntax should be: warmup2 [-lambda lambda] [-mu mu] [-r r] [-B B] [-P P] [-n num] [-t tsfile]\n");
                exit(1);
            }
            if(readnum < 0) {
                printf("-lambda is wrong, use default value\n");
            }
            else {
                lambda = readnum;
            }
        }
        else if(strcmp(argv[index], "-mu") == 0) {
            //printf("mu is %s\n", argv[++index]);
            //mu = atof(argv[++index]);
            
            double readnum = atof(argv[++index]);
            
            if(readnum == 0) {
                fprintf(stderr, "command line syntax should be: warmup2 [-lambda lambda] [-mu mu] [-r r] [-B B] [-P P] [-n num] [-t tsfile]\n");
                exit(1);
            }
            if(readnum < 0) {
                printf("-mu is wrong, use default value\n");
            }
            else {
                mu = readnum;
            }
        }

        else if(strcmp(argv[index], "-P") == 0) {
            //printf("P is %s\n", argv[++index]);
            //p = atoi(argv[++index]);
            
            int readnum = atoi(argv[++index]);
            if(readnum < 0 || readnum > 2147483647) {
                printf("-P is wrong, use default value\n");
            }
            else {
                p = readnum;
            }
            
        }
        else if(strcmp(argv[index], "-n") == 0) {
            //printf("num is %s\n", argv[++index]);
            //packet_count = atoi(argv[++index]);
            int readnum = atoi(argv[++index]);
            if(readnum == 0) {
                fprintf(stderr, "command line syntax should be: warmup2 [-lambda lambda] [-mu mu] [-r r] [-B B] [-P P] [-n num] [-t tsfile]\n");
                exit(1);
            }
            if(readnum < 0 || readnum > 2147483647) {
                printf("-n is wrong, use default value\n");
            }
            else {
                packet_count = readnum;
            }
        }

    }

    return NULL;

}

//initialize the packet with the give file
int initpck(FILE* fp, packet *pp) {

    //char word[20];
/*
    if(fgets(buf, sizeof(buf), fp) == NULL) {
        fprintf(stderr, "end of FILE!!!\n");
        exit(1);//clean up?
    }
*/

    //else {//buf is a line, interarrival request service (ms)

	//newp->inter_arrival_time = 1000000/lambda;

	fscanf(fp, "%s", buf);
        //printf("the inter_arrival_time is %d\n", atoi(buf));
	pp->inter_arrival_time = 1000*atoi(buf);

	fscanf(fp, "%s", buf);
        //printf("the request is %d\n", atoi(buf));
	pp->request = atoi(buf);

	fscanf(fp, "%s", buf);
        //printf("the inter_service_time is %d\n", atoi(buf));
	pp->service_time = 1000*atoi(buf);	

	//newp->service_time = 1000000/mu;
        //printf("the service_time is %d\n", newp->service_time);

	//newp->request = p;

    //}
    return 0;

}

int is_dir(char *path) {
    struct stat buf;
    stat(path, &buf);
    return S_ISDIR(buf.st_mode);
}
