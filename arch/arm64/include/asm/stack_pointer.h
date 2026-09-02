#ifndef __ASM_STACK_POINTER_H
#define __ASM_STACK_POINTER_H

/*
 * how to get the current stack pointer from C
 */
#define current_stack_pointer ({ \
	unsigned long current_sp; \
	asm ("mov %0, sp" : "=r" (current_sp)); \
	current_sp; \
})

#endif /* __ASM_STACK_POINTER_H */
