# Usermode Memory Manager
### Jason Wang
### Summer/Fall 2020

Memory management provides the underpinning for the entirety of the operating system.
Much as a bank may only hold a certain reserve requirement of liquid assets at any given time, the memory manager expands virtual memory beyond the bounds of the physical hardware
by implementing a complex state machine to create the illusion that memory allocated to system and user processes is exclusive, when in fact the physical memory itself may be reused.
The key distinction here is that in addition to creating the illusion of much greater memory capacity than that expected from the physical RAM, if correctly implemented, there is no loss of data.

This usermode virtual memory system incorporates key features of its kernel mode counterpart, including
* Paging
* Virtual addressing
* Physical memory management (by way of AWE)
* Page table hierarchies
* Multithreading and synchronization (page trimming/zeroing thread) 
