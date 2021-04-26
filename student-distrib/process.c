#include "process.h"
#include "file_sys.h"
#include "page_lib.h"
#include "lib.h"
#include "sys_call.h"
#include "scheduler.h"
#include "vidmap.h"

pcb_t* pcb_ptrs[N_PCB_LIMIT];
int32_t n_present_pcb;
pcb_t *focus_task_ = NULL;
pcb_t *foreground_task[MAX_TERMINAL];
terminal_struct_t *running_term = NULL;
terminal_struct_t* get_running_terminal() {
    return running_term;
}
void set_running_terminal (terminal_struct_t* cur) {
    running_term = cur;
}
/**
 * focus_task
 * Description: return the current focus task
 * Input: None
 * Output: focus_task
 */
pcb_t *focus_task() {
    if (focus_task_ == NULL) printf("No focus task\n");
    return focus_task_;
}

void process_user_vidmap(pcb_t *process) {
    // sanity check
    if (process == NULL) {
        printf("ERROR in process_user_vidmap(): NULL input");
        return;
    }
    if (process->vidmap_enable) {
        if (process->terminal == NULL) {
            printf("ERROR in process_user_vidmap(): the process allocates no terminal but enabled the vidmap");
            return;
        }
        set_video_memory(process->terminal);
    } else {
        set_video_memory(NULL); // the process disabled the vidmap
    }
}

/**
 * set_focus_task
 * Description: switch terminal
 * Input: terminal_num -- the terminal id want to change to
 * Output: focus_task
 */
void set_focus_task(pcb_t* target_task) {
    uint32_t flags;
    cli_and_save(flags);
    switch_terminal(target_task->terminal, focus_task()->terminal);
    focus_task_ = target_task;
    process_user_vidmap(get_cur_process());
    restore_flags(flags);
}

/**
 * change_focus_task
 * Description: switch terminal
 * Input: terminal_num -- the terminal id want to change to
 * Output: focus_task
 */
void change_focus_task(int32_t terminal_num) {
    set_focus_task(foreground_task[terminal_num]);
}

//#define execute_launch(kesp_des, new_esp, new_eip, ret) asm volatile ("                    \
//    pushfl          /* save flags */                                   \n\
//    pushl %%ebp     /* save EBP*/                                      \n\
//    pushl $1f       /* return address to label 1 after iret */          \n\
//    movl %%esp, %0                                                        \n\
//    pushl $0x002B                                                         \n\
//    pushl %2                                                            \n\
//    pushl $0x206                                                     \n\
//    pushl $0x0023                                                    \n\
//    pushl %3                                                          \n\
//    iret                                                              \n\
//1:  popl %%ebp                                                        \n\
//    movl %%eax, %1                                                     \n\
//    popfl   "                                              \
//    : "=m" (kesp_des),                                                            \
//      "=m" (ret)                                                                      \
//    : "rm" (new_esp), "rm" (new_eip)                                                  \
//    : "cc", "memory"                                                                  \
//)

#define execute_launch(kesp_save_to, new_esp, new_eip, ret) asm volatile ("                    \
    pushfl          /* save flags on the stack */                                   \n\
    pushl %%ebp     /* save EBP on the stack */                                     \n\
    pushl $1f       /* return address to label 1, on top of the stack after iret */ \n\
    movl %%esp, %0  /* save current ESP */                                          \n\
    /* The following stack linkage is for IRET */                                   \n\
    pushl $0x002B   /* user SS - USER_DS */                                         \n\
    pushl %2        /* user ESP */                                                  \n\
    pushl $0x206    /* flags (new program should not care, but IF = 1) */           \n\
    pushl $0x0023   /* user CS - USER_CS  */                                        \n\
    pushl %3        /* user EIP */                                                  \n\
    iret            /* enter user program */                                        \n\
1:  popl %%ebp      /* restore EBP, must before following instructions */           \n\
    movl %%eax, %1  /* return value pass by halt() in EAX */                        \n\
    popfl           /* restore flags */"                                              \
    : "=m" (kesp_save_to), /* must write to memory, or halt() will not get it */      \
      "=m" (ret)                                                                      \
    : "rm" (new_esp), "rm" (new_eip)                                                  \
    : "cc", "memory"                                                                  \
)

//#define execute_launch_in_kernel(kesp_des, new_esp, new_eip, ret) asm volatile (" \
//    pushfl                                                  \n\
//    pushl %%ebp                                                    \n\
//    pushl $1f                                                       \n\
//    movl %%esp, %0                                           \n\
//    movl %2, %%esp                                                \n\
//    pushl %3                                                   \n\
//    pushl $0x206   */                                                \n\
//    popfl                                                                           \n\
//    ret                                                        \n\
//1:  popl %%ebp                                                  \n\
//    movl %%eax, %1   */                                            \n\
//    popfl                           "                                              \
//    : "=m" (kesp_des), /* must write to memory, or halt() will not get it */      \
//      "=m" (ret)                                                                      \
//    : "r" (new_esp), "r" (new_eip)                                                    \
//    : "cc", "memory"                                                                  \
//)

#define execute_launch_in_kernel(kesp_save_to, new_esp, new_eip, ret) asm volatile (" \
    pushfl          /* save flags on the stack */                                   \n\
    pushl %%ebp     /* save EBP on the stack */                                     \n\
    pushl $1f       /* return address to label 1, on top of the stack after iret */ \n\
    movl %%esp, %0  /* save current ESP */                                          \n\
    movl %2, %%esp  /* set new ESP */                                               \n\
    pushl %3        /* new EIP */                                                   \n\
    pushl $0x206    /* flags (new program should not care, but IF = 1) */           \n\
    popfl                                                                           \n\
    ret             /* enter new kernel task */                                     \n\
1:  popl %%ebp      /* restore EBP, must before following instructions */           \n\
    movl %%eax, %1  /* return value pass by halt() in EAX */                        \n\
    popfl           /* restore flags */"                                              \
    : "=m" (kesp_save_to), /* must write to memory, or halt() will not get it */      \
      "=m" (ret)                                                                      \
    : "r" (new_esp), "r" (new_eip)                                                    \
    : "cc", "memory"                                                                  \
)

/**
 * load_esp_and_return
 * Description: reload esp then ret to the address on kernel stack
 * Input: new_esp - the stack we want to switch to
 *        status - value to load in eax
 * Output: None
 * Side effect: Switch kernel stack
 */
//#define load_esp_and_return(new_esp,status) asm volatile ("                  \
//        movl %0, %%esp  /* load old ESP */                                           \n\
//        ret  /* now it's equivalent to jump execute return */"                         \
//        :                                                                              \
//        : "r" (new_esp)   , "a" (status)                                         \
//        : "cc", "memory"                                                               \
//    )

#define load_esp_and_return(old_esp, ret) asm volatile ("                                        \
    movl %0, %%esp  /* load back old ESP */                                      \n\
    /* EAX store the return status */                                            \n\
    ret  /* on the old kernel stack, return address is on the top of the stack */" \
    :                                                                              \
    : "r" (old_esp)   , "a" (ret)                                                  \
    : "cc", "memory"                                                               \
)

/**
 * parse_args
 * Description: parse the input character to
 * obtain separate command and args
 * Input: command -- string to be splited
 * Output: args -- pointer to argument string
 *         command -- name of execuatable command
 * Side effect: None
 */
int parse_args(uint8_t *command, uint8_t **args){
    // bad input
    if (command == NULL) {
        printf("ERROR in parse_args: command NULL pointer\n");
        return -1;
    }
    // Possible to have no arguments
    *args = NULL;
    while(*command !='\0') {
        if (*command == ' ') {
            *command = '\0'; // End here
            *args = command + 1;
            return 0;
        }
        ++command;
    }
    return 0;
}
int get_another_terminal_id(int id) {
    int i;
    for (i = 1; i < MAX_TERMINAL; i++) {
        if (foreground_task[(id - i + MAX_TERMINAL) % MAX_TERMINAL] != NULL) {
            return (id - i + MAX_TERMINAL) % MAX_TERMINAL;
        }
    }
    return -1;
}
/**
 * sys_execute
 * Description: execute the command
 * Input: command -- string to be executed
 *        wait_for_child -- if set to 1, means it waits for the child to return
 *                          if set to 0, indicate it's an init task
 *                          if set to -1, create an idle task
 *        separate_terminal -- if set to 1 means new terminal is used for this process
 *        function_address -- NULL means execute by args, else means execute kernel thread
 * Output: None
 * Side effect: Run the new program with the command
 */

int32_t sys_execute(uint8_t *command, int wait_for_child, int separate_terminal, void (*function_address)()) {
    pcb_t *process;
    uint32_t eip;
    uint32_t length;
    int pid_ret;
    int ret;

    // bad input
    if (command == NULL) {
        printf("ERROR in sys_execute: command NULL pointer\n");
        return -1;
    }
    // set up pcb for the new task
    process = create_process();
    if (process==NULL) return -1; // Raise Error
    process->init_task=process->kernel_task=process->idle_task=process->own_terminal=process->wait_for_child=0;
    // Information Setting
    if (get_n_present_pcb()==1) {
        process->init_task = 1;
        process->parent = NULL;
    } else {
        process->parent = (wait_for_child==1)?get_cur_process():NULL;
    }
    if (function_address!=NULL) process->kernel_task=1;
    if (wait_for_child==-1) process->idle_task=1;
    process->vidmap_enable = 0;
    process->k_esp = (uint32_t)process+TASK_KSTK_SIZE_IN_B-1;
    // Parse name and arguments
    parse_args(command, &(process->args));
    process->name = command;
    // move name and argument to new program's PCB
    length = strlen((int8_t *)process -> name);
    process->k_esp-=length;
    process->name=(uint8_t *)strcpy((int8_t *)process->k_esp, (int8_t *) process->name);
    if (process->args!=NULL) {
        length = strlen((int8_t *)process -> args);
        process->k_esp-=length;
        process->args=(uint8_t *)strcpy((int8_t *)process->k_esp, (int8_t *) process->args);
    }
    if (add_task_to_list(process)==-1) return -1;
    //  set video memory for the current process
    if (process->kernel_task) {
        process -> terminal = NULL;
        process -> pid = -1; // kernel task has no paging and terminal
        eip = (uint32_t) function_address;
    }  else {
        if (separate_terminal) {
            process->terminal = terminal_allocate();
            terminal_turn_on(process->terminal);
        } else {
            if (process->init_task) process->terminal = NULL;
            else process->terminal = get_cur_process()->terminal; // Use the caller's terminal
        }
    }
    pid_ret = set_page_for_task(command, (uint32_t *)&eip);
    if (pid_ret < 0) {
        delete_process(process);
        return -1;
    }
    process->pid = pid_ret;
    if (process->terminal != NULL) {   
        foreground_task[process->terminal->id] = process; // Become the task using terminal
        set_focus_task(process);
    }
    terminal_set_running(process->terminal);
    if (separate_terminal) {
        clear();
        reset_screen();
        printf("TERMINAL %d\n", process->terminal->id);
    }

    init_file_arr(&(process->file_arr));
    task_signal_init(&(process->signals));
    // Set up tss to make sure system call don't go wrong
    // Set up Scheduler
    if (process -> idle_task) process->time = 0;
    else init_process_time(process);
    insert_to_list_start(process->node);
    if (wait_for_child==1) {
        process->wait_for_child=1;
        reposition_to_end(process->node);
    }
    tss.esp0 = process->k_esp;
    if (process->kernel_task) {
        if (process->init_task) {
            execute_launch_in_kernel(length, process->k_esp, eip, ret);
        } else {
            execute_launch_in_kernel(get_cur_process()->k_esp, process->k_esp, eip, ret);
        }
    } else {
        if (process->init_task) {
            execute_launch(length, US_STARTING, eip, ret);
        } else {
            execute_launch(get_cur_process()->k_esp, US_STARTING, eip, ret);
        }
    }
    return ret;
}

/**
 * system_halt
 * Description: Implementation of halt()
 * Input: status - exit status info
 * Output: 0 if success.
 * Return: should never return
 * Side effect: terminate the process and return value to its parent
 */

int32_t system_halt(int32_t status) {


    pcb_t * cur_task = get_cur_process();
//    task_node * cur_node = cur_task->node;

    // condition check
    // TODO: check whether it works
    if (get_n_present_pcb() == 0) {   // not in a process

        // clear page directory for video memory
        clear_video_memory();
        sys_execute((uint8_t *) "shell", 0, 1, NULL);

        return -1;
    }


    // remove cur_task from task list
    delete_task_from_list(cur_task);

    // if cur_task has parent, we simply switch to parent task
    if (cur_task->parent != NULL) {
        // TODO: why init time for parent?
        init_process_time(cur_task->parent);
        insert_to_list_start(cur_task->parent->node);
    }

    // close file array

    // TODO: manipulate terminal
    // ---------------------------------------
    int term_id = cur_task->terminal->id;
    // Perform the deallocation of terminal
    if (cur_task->own_terminal) {
        foreground_task[term_id] = NULL;
        if (focus_task_->terminal->id == term_id) {
            int new_term_id = get_another_terminal_id(term_id);
            if (new_term_id != -1) {
                set_focus_task(foreground_task[new_term_id]);
            }
        }
        terminal_deallocate(cur_task->terminal);
    } else {
        foreground_task[cur_task->parent->terminal->id] = cur_task->parent;
        if (focus_task_->terminal->id == cur_task->parent->terminal->id) {
            set_focus_task(cur_task->parent);
        }
    }

    // ---------------------------------------


    // no parent, we just reschedule
    if(cur_task->parent == NULL){
        close_all_files(&cur_task->file_arr);
        delete_process(cur_task);
        reschedule();
    } else {
        close_all_files(&cur_task->file_arr);
        delete_process(cur_task);
        // we have parent, so just switch to them
        restore_paging(cur_task->pid,cur_task->parent->pid);

        // TODO: terminal
        // ---------------------------------------------
        terminal_set_running(cur_task->parent->terminal);
        // ---------------------------------------------

        tss.esp0 = cur_task->parent->k_esp;
        load_esp_and_return(cur_task->parent->k_esp, status);
        }

        printf("In system_halt: this function should never return.\n");
        return -1;
    }

/**
 * process_init
 * Description: Initialize the process array system
 * Input: None
 * Output: 0 if success.
 * Side effect: all processes are set not present
 */
void process_init() {
    int i;
    n_present_pcb = 0;
    for (i = 0; i < N_PCB_LIMIT; i++) {
        pcb_ptrs[i] = (pcb_t*) (TASK_KSTK_BOTTOM - (i+1) * TASK_KSTK_SIZE_IN_B);
        pcb_ptrs[i]->present = 0;   // init as not present
    }
    for (i = 0; i < MAX_TERMINAL; ++i) foreground_task[i] = NULL;
    init_scheduler();
}

void run_initial_task() {
    uint32_t flags;
    cli_and_save(flags);
    {
        sys_execute((uint8_t *) "initial", 0, 0, init_task_main);
    }
    restore_flags(flags);
}
/**
 * create_process
 * Description: create a new pcb and return the pcb pointer
 * Input: None
 * Output: the pointer to the new pcb
 * Side effect: everything in the pcb is not initialized
 */
pcb_t* create_process() {
    int i;

    if (n_present_pcb >= N_PCB_LIMIT) {
        printf("ERROR in create_process: number of process limit reached\n");
        return NULL;
    }
    n_present_pcb += 1;

    for (i = 0; i < N_PCB_LIMIT; i++) {
        if (!(pcb_ptrs[i]->present)) {
            pcb_ptrs[i]->present = 1;
            return pcb_ptrs[i];
        }
    }

    printf("ERROR in create_process: somethings wrong\n");
    return NULL;
}


/**
 * get_cur_process
 * Description: obtain the current running process
 * Input: None
 * Output: the address of the current pcb
 * Side effect: None
 */
pcb_t* get_cur_process() {
    pcb_t *ret;
    asm volatile (
        "movl %%esp, %0" \
        : "=r" (ret) \
        : \
        : "cc", "memory" \
    );
    ret = (pcb_t*) (((int32_t) ret) & TASK_KSTK_PCB_ADDR_MASK);
    return ret;
}

/**
 * delete_process
 * Description: delete the process
 * Input: the pcb of target process
 * Output: the parent of the deleted process
 * Side effect: the process is deleted
 */
pcb_t* delete_process(pcb_t* pcb) {
    if (pcb == NULL) {
        printf("ERROR in delete_process: pcb NULL pointer\n");
        return NULL;
    }
    pcb->present = 0;
    n_present_pcb -= 1;
    return pcb->parent;
}

/**
 * get_n_present_pcb
 * Description: get the number of current present process
 * Input: None
 * Output: n_present_pcb
 * Side effect: None
 */
int32_t get_n_present_pcb() {
    return n_present_pcb;
}

void idle_task_main() {
    while (1) {}
}

void init_task_main() {

    int32_t ret;
    uint32_t flags;
    cli_and_save(flags);
    {
        sys_execute((uint8_t *) "idle", -1, 0, idle_task_main);
        sys_execute((uint8_t *) "shell", 0, 1, NULL);
        sys_execute((uint8_t *) "shell", 0, 1, NULL);
        do {
            ret = sys_execute((uint8_t *) "shell", 1, 1, NULL);
        } while (ret == 0);
        system_halt(-1);
    }
    restore_flags(flags);
}


