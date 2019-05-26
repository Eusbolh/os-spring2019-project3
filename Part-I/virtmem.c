/**
 * virtmem.c 
 * Written by Michael Ballantyne 
 * Modified by Didem Unat
 */

#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#define TLB_SIZE 16
#define PAGES 256
#define PAGE_MASK 255
#define PAGE_SIZE 256
#define OFFSET_BITS 8
#define OFFSET_MASK 255

/**
 * Number of pages is not equal to number of frames, therefore
 * we need another constant for number of frames. In addition,
 * calculation of 'MEMORY_SIZE' must be changes as well.
 */
#define FRAMES 64
#define MEMORY_SIZE FRAMES * PAGE_SIZE // TODO: Changes PAGES->FRAMES
#define VIRTUAL_MEMORY_SIZE PAGES * PAGE_SIZE

// Max number of characters per line of input file to read.
#define BUFFER_SIZE 10

struct tlbentry {
  unsigned char logical;
  unsigned char physical;
};

// TLB is kept track of as a circular array, with the oldest element being overwritten once the TLB is full.
struct tlbentry tlb[TLB_SIZE];
// number of inserts into TLB that have been completed. Use as tlbindex % TLB_SIZE for the index of the next TLB line to use.
int tlbindex = 0;

/**
 * We changed the array implementation from 1D to 2D to keep valid/invalid bit.
 * pagetable[logical_page][0] is the physical page number for logical page. (-1 if logical page is not in the physical memory)
 * pagetable[logical_page][1] is the valid/invalid bit for logical page. (0 if logical page is not in the physical memory)
 */
int pagetable[PAGES][2];

signed char main_memory[MEMORY_SIZE];

// Pointer to memory mapped backing file
signed char *backing;

/* *** */

/* DLL Queue Implementation */

typedef struct QNode {
  struct QNode *prev, *next;
  unsigned key;
} QNode;

typedef struct Queue {
  int size, capacity;
  QNode *front, *rear;
} Queue;

QNode* newQNode(int key) {
  QNode* temp = (QNode *)malloc(sizeof( QNode )); 
  temp->key = key;
  temp->prev = temp->next = NULL;

  return temp;
}

Queue* createQueue(int capacity) {
  Queue* queue = (Queue *)malloc(sizeof( Queue )); 
  queue->size = 0; 
  queue->front = queue->rear = NULL; 
  queue->capacity = capacity; 
  return queue;
}

int isFull(Queue* queue) { return queue->size == queue->capacity; }

int isEmpty(Queue* queue) { return queue->rear == NULL; }

int isIncluded(Queue* queue, int key) {
  QNode* temp = queue->front;
  while (temp != NULL) {
    if (temp->key == key) {
      return true;
    }
    temp = temp->next;
  }
  return false;
}

int dequeue(Queue* queue) {
  if (isEmpty(queue)) return;
  if (queue->front == queue->rear)
    queue->front = NULL;
  QNode* temp = queue->rear;
  int key = temp->key;
  queue->rear = queue->rear->prev; 
  if (queue->rear) 
    queue->rear->next = NULL; 
  free(temp);  
  queue->size--;
  return key;
}

void enqueue(Queue* queue, int key) { 
  if (isFull(queue)) {
    printf("Size: %d, Capacity: %d\n", queue->size, queue->capacity);
    printf("Queue is full!\n");
    return;
  } 
  QNode* temp = newQNode( key ); 
  temp->next = queue->front; 
  if (isEmpty(queue)) {
    queue->rear = queue->front = temp; 
  } else { 
    queue->front->prev = temp; 
    queue->front = temp; 
  } 
  queue->size++; 
} 

/* *** */

void referencePage(Queue* queue, int logical_page) {
  if(isIncluded(queue, logical_page)) {
    if (queue->front->key == logical_page) {
      // Do nothing
    } else if (queue->rear->key == logical_page) {
      dequeue(queue);
      enqueue(queue, logical_page);
    } else {
      QNode* temp = queue->front->next;
      while(temp->key != queue->rear->key) {
        if (temp->key == logical_page) {
          temp->prev->next = temp->next;
          temp->next->prev = temp->prev;
          queue->size--;
          enqueue(queue, logical_page);
          break;
        }
        temp = temp->next;
      }
    }
  } else {
    // Code shouldn't be reached here!
  }
}

void printQueue(Queue* queue) {
  QNode* temp = queue->front;
  int counter = 0;
  while (temp != NULL) {
    printf("#%d: %d\n", counter++, temp->key);
    temp = temp->next;
  }
}

unsigned char next_frame = 0;
unsigned char current_free_frame = 0;

unsigned char getFreeFrame(Queue* queue, int mode, int logical_page) {

  /* Case 1: There is free frame */
  if (!isFull(queue)) {
    // printf("There is frame!\n");
    enqueue(queue, logical_page);
    pagetable[logical_page][0] = next_frame;
    pagetable[logical_page][1] = 1;
    return next_frame++;
  }
  /* Case 2: Given page is not in the set */
  else if (!isIncluded(queue, logical_page)) {
    //printf("There is no more frame and the page is not in the set!\n");
    int replaced_page = dequeue(queue);
    // printf("Replaced Page: %d\n", replaced_page);
    // printf("isIncluded: %d\n", isIncluded(queue, replaced_page));
    enqueue(queue, logical_page);
    int free_frame = pagetable[replaced_page][0];
    pagetable[replaced_page][0] = -1;
    pagetable[replaced_page][1] = 0;
    pagetable[logical_page][0] = free_frame;
    pagetable[logical_page][1] = 1;
    return free_frame;
  }
}

int max(int a, int b)
{
  if (a > b)
    return a;
  return b;
}

/* Returns the physical address from TLB or -1 if not present. */
int search_tlb(unsigned char logical_page) {
  int i;
  for (i = max((tlbindex - TLB_SIZE), 0); i < tlbindex; i++) {
    struct tlbentry *entry = &tlb[i % TLB_SIZE];
    
    if (entry->logical == logical_page) {
      return entry->physical;
    }
  }
  
  return -1;
}

/* Adds the specified mapping to the TLB, replacing the oldest mapping (FIFO replacement). */
void add_to_tlb(unsigned char logical, unsigned char physical) {
  struct tlbentry *entry = &tlb[tlbindex % TLB_SIZE];
  tlbindex++;
  entry->logical = logical;
  entry->physical = physical;
}

int main(int argc, const char *argv[])
{
  /* Mode Variable */
  int mode = -1;

  /* Initializing Queue */
  Queue* queue = createQueue(FRAMES);
  printf("Size: %d, Capacity: %d\n", queue->size, queue->capacity);

  /* Validating arguments */
  bool terminate = false;
  if (argc != 5 || strcmp(argv[3], "-p") != 0 || atoi(argv[4]) > 1 || atoi(argv[4]) < 0) {
    printf("\033[1;31m[ERROR] Correct usage: ./virtmem BACKING_STORE.bin addresses.txt -p 0.\033[0m\n");
    return 0;
  } else {
    mode = atoi(argv[4]);
  }
  
  const char *backing_filename = argv[1]; 
  int backing_fd = open(backing_filename, O_RDONLY);
  backing = mmap(0, VIRTUAL_MEMORY_SIZE, PROT_READ, MAP_PRIVATE, backing_fd, 0); 
  
  const char *input_filename = argv[2];
  FILE *input_fp = fopen(input_filename, "r");
  
  // Fill page table entries with -1 for initially empty table.
  int i;
  for (i = 0; i < PAGES; i++) {
    pagetable[i][0] = -1;
    pagetable[i][1] = 0;
  }
  
  // Character buffer for reading lines of input file.
  char buffer[BUFFER_SIZE];
  
  // Data we need to keep track of to compute stats at end.
  int total_addresses = 0;
  int tlb_hits = 0;
  int page_faults = 0;
  
  // Number of the next unallocated physical page in main memory
  unsigned char free_page = 0;
  
  while (fgets(buffer, BUFFER_SIZE, input_fp) != NULL) {
    total_addresses++;
    int logical_address = atoi(buffer);
    int offset = logical_address & OFFSET_MASK;
    int logical_page = (logical_address >> OFFSET_BITS) & PAGE_MASK;
    
    int physical_page = search_tlb(logical_page);
    // TLB hit
    if (physical_page != -1) {
      tlb_hits++;
      // TLB miss
    } else {
      if (pagetable[logical_page][1] == 0) {
        /* CASE 1: Page fault occured */
        
        /* Incrementing the page faults counter */
        page_faults++;

        /* Getting a free frame (replacing a one if necessary) */
        physical_page = getFreeFrame(queue, mode, logical_page);

        /* Copying the content of page into memory */
        memcpy(main_memory + physical_page * PAGE_SIZE, backing + logical_page * PAGE_SIZE, PAGE_SIZE);

        /* Updating Page Table */
        pagetable[logical_page][0] = physical_page;
        pagetable[logical_page][1] = 1;
      }
      else {
        /* CASE 2: Page fault did not occur */

        /* Getting the frame number from page table */
        physical_page = pagetable[logical_page][0];
      }

      
      /*
      // Page fault
      if (physical_page == -1) {
        page_faults++;
              
        physical_page = free_page;
        free_page++;
        
        // Copy page from backing file into physical memory
        memcpy(main_memory + physical_page * PAGE_SIZE, backing + logical_page * PAGE_SIZE, PAGE_SIZE);
        
        pagetable[logical_page][0] = physical_page;
      }*/
      
      add_to_tlb(logical_page, physical_page);
    }
    if (mode == 1)
      referencePage(queue, logical_page);
    
    int physical_address = (physical_page << OFFSET_BITS) | offset;
    signed char value = main_memory[physical_page * PAGE_SIZE + offset];
    
    // printf("Virtual address: %d Physical address: %d Value: %d\n", logical_address, physical_address, value);
  }
  
  printf("Number of Translated Addresses = %d\n", total_addresses);
  printf("Page Faults = %d\n", page_faults);
  printf("Page Fault Rate = %.3f\n", page_faults / (1. * total_addresses));
  printf("TLB Hits = %d\n", tlb_hits);
  printf("TLB Hit Rate = %.3f\n", tlb_hits / (1. * total_addresses));
  
  return 0;
}
