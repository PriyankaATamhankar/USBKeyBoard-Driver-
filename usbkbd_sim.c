#include<pthread.h>
#include<semaphore.h>
#include<stdlib.h>
#include<ctype.h>
#include<stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include<unistd.h>
#include<stdio.h>
#include<sys/mman.h>

pthread_t kbd_irq;
pthread_t kbd_ctl;
sem_t slock;

int irq_fd[2];
int ack_fd[2];
int ctl_fd[2];
int* ledsbuff;
int stat_open = 0;
int in_stop = 0;

struct usb_kbddev;
typedef void* (*eventhandler)(void* urb); //function pointer for event handling

struct usb_kbddev //input_dev
    {
        int pipe;
        char* data_buffer;
        char* prev;
        eventhandler event_handler;
    };
    
struct usb_kbd
    {
        struct usb_kbddev* urb_irq;
        struct usb_kbddev* urb_led;
        sem_t lock_led;
        sem_t submit_led;
        int CAPS_MODE;
        int pendingLED;
        int irqsub_flag;
        int ledsub_flag;
    }*my_usbdev;
    
void* poll_IRQ(void*my_urb_irq) //usb core polling function
    {
         pthread_t irqThread;
         struct usb_kbddev* temp_urb_irq=(struct usb_kbddev*)my_urb_irq;
		while(read(temp_urb_irq->pipe,temp_urb_irq->data_buffer,1))
			{
			    if(pthread_create(&irqThread,NULL,temp_urb_irq->event_handler,(void*)my_urb_irq)<0)
				perror(" IRQ handler thread creation failed\n");
			    pthread_join(irqThread,NULL);
			}
        close(temp_urb_irq->pipe);
        exit(0);
    }
    
void* poll_ACK(void* my_URBled) //usb core polling function
    {   
        char c='P';
        char* buff=&c;
        struct usb_kbddev* temp_urb_led=(struct usb_kbddev*)my_URBled;
        while(true)
		{
		    sem_wait(&(my_usbdev->submit_led));
		    write(ctl_fd[1],buff,1);
		    pthread_t ledThread;
		    if(read(temp_urb_led->pipe,temp_urb_led->data_buffer,1))
			    {
				if(pthread_create(&ledThread,NULL,temp_urb_led->event_handler,(void*)my_URBled) == -1)
				    perror(" CTL handler thread creation failed\n");
				pthread_join(ledThread,NULL);
			    }
		    else
		        break;
		}
        close(ctl_fd[1]);
        close(temp_urb_led->pipe);
    }    

int submit_urb(struct usb_kbddev* my_urb_irq, struct usb_kbddev* my_URBled)  //submit urb to usb core. Creates polling threads when called first time
    {
        sem_wait(&slock);
        if(!stat_open)
		{
		    stat_open = 1;
		    sem_post(&slock);
	 
		    if(pthread_create(&kbd_irq,NULL,poll_IRQ,(void*)my_urb_irq)<0)
		    	perror(" poll_irq fucntion thread creation failed\n");		    	
		    if(pthread_create(&kbd_ctl,NULL,poll_ACK,(void*)my_URBled)<0)
		    	perror(" poll_ack fucntion thread creation failed\n");
		    pthread_join(kbd_irq,NULL);
		    pthread_join(kbd_ctl,NULL);
		    exit(0);
		}
        sem_post(&slock);
        sem_wait(&my_usbdev->lock_led);
        my_usbdev->ledsub_flag=1;
        sem_post(&my_usbdev->lock_led);
        sem_post(&my_usbdev->submit_led);
        return 0;
    }


void* usb_kbd_event() //handles capslock event
    {
        sem_wait(&my_usbdev->lock_led);
        if(my_usbdev->ledsub_flag==1)
		{
		    my_usbdev->pendingLED+=1;
		    sem_post(&my_usbdev->lock_led);
		}
        *ledsbuff=(*ledsbuff+1)%2;
        sem_post(&my_usbdev->lock_led);
        submit_urb(NULL,my_usbdev->urb_led);
    }

void input_report_key(char key) //custom changes in keyboard according to the special characters received
    {
        
        switch (key)
        {
            case '#': 
		        if(*my_usbdev->urb_irq->prev=='@')
				{
				    printf("%c",*my_usbdev->urb_irq->prev);
				    my_usbdev->urb_irq->prev='\0';
				}
		        break;
            case '&':
                
		        if(*my_usbdev->urb_irq->prev=='@')
				{
				    my_usbdev->CAPS_MODE=(my_usbdev->CAPS_MODE+1)%2;
				    *my_usbdev->urb_irq->prev='\0';
				    pthread_t usb_event;
				    if(pthread_create(&usb_event,NULL,usb_kbd_event,NULL)<0)
				        printf("event thread not created\n");
				    
				    return;
				}
		        printf("%c\n",key);
		        break; 

            case '@':
		    if(*my_usbdev->urb_irq->prev=='@')
			    {
				printf("%c",*my_usbdev->urb_irq->prev);
				my_usbdev->urb_irq->prev="\0";
			    }
		    *my_usbdev->urb_irq->prev='@';
		     break;
            
            default :  
		    if(*(my_usbdev->urb_irq->prev)=='@')
			    {
				printf("%c",*my_usbdev->urb_irq->prev);
				*my_usbdev->urb_irq->prev='\0';
			    }
			    if(my_usbdev->CAPS_MODE)
				    {
					printf("%c",toupper(key));
					return;
				    }
			    printf("%c",key);
		    break;
        }
    }


void* usb_kbd_irq(void* my_urb_irq) //completion handler for irq
    {
        struct usb_kbddev* my_urb_irq1=(struct usb_kbddev*)my_urb_irq;
        input_report_key(*my_urb_irq1->data_buffer);
    }

void* usb_kbd_led(void* my_URBled) //completion handler for led
    {
        sem_wait(&my_usbdev->lock_led);
		if(my_usbdev->pendingLED==0)
			{
			    my_usbdev->ledsub_flag=0;
			    sem_post(&my_usbdev->lock_led);
			}
		else
			{
			    my_usbdev->pendingLED--;
			    *ledsbuff=(*ledsbuff+1)%2;
			    sem_post(&my_usbdev->lock_led);
			    submit_urb(NULL,my_usbdev->urb_led);
			}
    }
    
    
void* irq_endpoint() //iqr endpoint of the keyboard 
    {
        char* buf=(char*)malloc(sizeof(char));
        printf("\n");
        while(read(STDIN_FILENO,buf,1))
		{
		    write(irq_fd[1],buf,1);
		    sleep(0.4);
		}
        close(irq_fd[1]);
    }

void* ctl_endpoint() //control endpoint of the keyboard
    {   
        char* tempbuf=(char*)malloc(sizeof(char));
        char* caps_buf=(char*)malloc(sizeof(char));
        
        int cnt=1,i=0;
        char tempwbuf;
        while(read(ctl_fd[0],tempbuf,1))
             {
                caps_buf=(char*)realloc(caps_buf,cnt*sizeof(char));
                if(!(*ledsbuff))
                    caps_buf[cnt-1] = '0';
                else
                    caps_buf[cnt-1] = '1';
                
                write(ack_fd[1],&tempwbuf,1);
                cnt++;
             }
        close(ack_fd[1]);
        close(ctl_fd[0]);
        
        while(i<cnt-1)
            {
                if(caps_buf[i] == '0')
                    printf("OFF ");
                else
                    printf("ON ");
                i++;
            }
		printf("\n");
    }

int usb_kbd_open()  //initialize device data structure
    {
        my_usbdev=(struct usb_kbd*)malloc(sizeof(struct usb_kbd));
        my_usbdev->urb_irq=(struct usb_kbddev*)malloc(sizeof(struct usb_kbddev));
        my_usbdev->urb_led=(struct usb_kbddev*)malloc(sizeof(struct usb_kbddev));
        
        my_usbdev->urb_irq->data_buffer = (char*)malloc(sizeof(char));
        my_usbdev->urb_led->data_buffer = (char*)malloc(sizeof(char));
        
        my_usbdev->urb_irq->event_handler = &usb_kbd_irq;
        my_usbdev->urb_irq->prev = (char*)malloc(sizeof(char));
        *my_usbdev->urb_irq->prev = '\0';
        my_usbdev->urb_led->event_handler = &usb_kbd_led;
        my_usbdev->urb_led->prev=(char*)malloc(sizeof(char));
        *my_usbdev->urb_led->prev='\0';
        
        my_usbdev->urb_irq->pipe = irq_fd[0];
        my_usbdev->urb_led->pipe = ack_fd[0];
        
        my_usbdev->irqsub_flag=1;
        my_usbdev->ledsub_flag=0;
        my_usbdev->CAPS_MODE=0;
        my_usbdev->pendingLED=0;
        
        sem_init(&(my_usbdev->lock_led),0,1);
        if(sem_init(&(my_usbdev->submit_led),0,0) == 0)
		{
		    sem_post(&(my_usbdev->submit_led));
		    sem_wait(&(my_usbdev->submit_led));
		}
        submit_urb(my_usbdev->urb_irq, my_usbdev->urb_led);
        return 0;
    }

int main()
    {
        sem_init(&slock,0,1);
        ledsbuff=(int*)mmap(NULL,1,PROT_READ | PROT_WRITE,MAP_SHARED | MAP_ANONYMOUS,0,0); 
        *ledsbuff=0;
        
      
        if(pipe(irq_fd)<0)
		  perror("IRQ PIPE CREATION FAILED");
        if(pipe(ack_fd)<0)
		  perror("ACK PIPE CREATION FAILED");
        if(pipe(ctl_fd)<0)
		  perror("CNTL PIPE CREATION FAILED");

       int child_pid = fork();
       
       if(child_pid == -1)
	     perror("Keyboard process creation failed");
       else if(child_pid == 0)
	       {
	        close(irq_fd[0]);
                close(ack_fd[0]);
                close(ctl_fd[1]);
                pthread_t kbd_irq_endpoint;
                pthread_t kbd_ctl_endpoint;
		        if(!pthread_create(&kbd_irq_endpoint,NULL,irq_endpoint,NULL)<0)
		            perror("kbd_irq_endpoint creation failed");
		        
		        if(pthread_create(&kbd_ctl_endpoint,NULL,ctl_endpoint,NULL)<0)
		             perror("kbd_ctl_endpoint creation failed");
                
                if(pthread_join(kbd_irq_endpoint,NULL)==-1)
                	perror("Failed to join kbd_irq endpoint");
                	
                if(pthread_join(kbd_ctl_endpoint,NULL)==-1)
                	perror("Failed to join kbd_ctl endpoint");
	       }
       else
	       {
	           close(irq_fd[1]);
                  close(ack_fd[1]);
                  close(ctl_fd[0]);
                  usb_kbd_open(); // starting application by calling usb_kbd_open
	       }
        return 0;
    }
