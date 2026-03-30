#include "userapp.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#define PROC_FILE "/proc/mp2/status"

void do_job(unsigned long loops)
{
    volatile unsigned long i;
	for(i=0;i<loops;i++);
}

long long get_ms(struct timespec *t0,struct timespec *t1)
{
	return (t1->tv_sec-t0->tv_sec)*1000LL+(t1->tv_nsec-t0->tv_nsec)/1000000LL;
}

int main(int argc,char *argv[])
{
	int fd,i,period,comp,jobs=5,n,admitted=0;
	unsigned long loops;
	pid_t pid;
	char buf[128],readbuf[4096],pidstr[32];
	struct timespec t0,start,end;

	if(argc!=4)
	{
		printf("Usage: %s <period> <comp> <loops>\n",argv[0]);
		return 1;
	}

	period=atoi(argv[1]);
	comp=atoi(argv[2]);
	loops=atol(argv[3]);
	pid=getpid();

	sprintf(buf,"R,%d,%d,%d",pid,period,comp);
	fd=open(PROC_FILE,O_WRONLY);
	write(fd,buf,strlen(buf));
	close(fd);

	fd=open(PROC_FILE,O_RDONLY);
	n=read(fd,readbuf,sizeof(readbuf)-1);
	close(fd);
	if(n>=0)
	{
		readbuf[n]='\0';
		sprintf(pidstr,"%d:",pid);
		if(strstr(readbuf,pidstr)!=NULL) admitted=1;
	}

	if(!admitted)
	{
		printf("PID %d not admitted\n",pid);
		return 1;
	}

	printf("success");

	clock_gettime(CLOCK_MONOTONIC,&t0);

	sprintf(buf,"Y,%d",pid);
	fd=open(PROC_FILE,O_WRONLY);
	write(fd,buf,strlen(buf));
	close(fd);

	for(i=0;i<jobs;i++)
	{
		clock_gettime(CLOCK_MONOTONIC,&start);
		printf("[PID %d] job %d start at %lld ms\n",pid,i,get_ms(&t0,&start));

		do_job(loops);

		clock_gettime(CLOCK_MONOTONIC,&end);
		printf("[PID %d] job %d finish at %lld ms, runtime=%lld ms\n",pid,i,get_ms(&t0,&end),get_ms(&start,&end));

		sprintf(buf,"Y,%d",pid);
		fd=open(PROC_FILE,O_WRONLY);
		write(fd,buf,strlen(buf));
		close(fd);
	}

	sprintf(buf,"D,%d",pid);
	fd=open(PROC_FILE,O_WRONLY);
	write(fd,buf,strlen(buf));
	close(fd);
	return 0;
}