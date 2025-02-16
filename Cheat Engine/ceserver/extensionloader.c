/*
 * extentionloader.c
 *
 *  Created on: Aug 19, 2013
 *      Author: eric
 *
 *  Used for loading an module that will extend the ceserver
 *  client<-->ceserver<-->extention
 *
 *  It doesn't HAVE to be used if the forced module load method works (Do not assume so)
 *
 *  How it works:
 *  Ptrace the target  (this means it must be loaded BEFORE the debugger is attached)
 *  Cause a stop and make sure it's runnable (Not sure if it executes if it's suspended for some reason. e.g: wait for event/critical section that may never happen)
 *  Change the current instruction pointer to the beginning of dlopen and the register/stack state setup to execute
 *  Set the return addres to an invalid return address (e.g 0x0ce0)
 *  Execute it and wait till a sigtrap/sigseg happens on that specific invalid address
 *  Then restore the state back
 *
 *  On arm32: Bit J and T in CPSR define the current execution state
 *  J T
 *  0 0 = ARM
 *  0 1 = Thumb
 *  1 0 = Jazelle (java...)
 *  1 1 = ThumbEE*
 *
 *  If ARM so set to 0 0 and restore that as well
 *  Note that The least significant bit in an address specifier also determines if it's THUMB or ARM (edit: nope.  But lets go with this in CE)
 *  It doesn't seem to matter if you set the least significant bit in the PC register. It will ignore that but on execute. (probably a good idea to clear that bit anyhow)
 *
 *
 *  Problem: It doesn't return properly when the registers are changed when it's waiting in a syscall, so only change it when outside of a syscall
 *  Better solution: It seems it failed because the stop was at a syscall, so the program counter was decremented tithe the size of the syscall
 *  To prevent this RESTART change EAX to 0 so it won't do the restart.  Also works on ARM
 *
 *  Problem2: In android dlopen is in /system/bin/linker but not using a symbol (so ce's symbollist can't be used to find the address)
 *
 *  dlopen("libdl.so", RTLD_NOW) actually works in android and dlsym as well. (point to the linker version)
 *  This is useful since this makes it cross compatible with normal linux.
 *  for some reason getting the address of dlopen in x86 returns a local stub and I don't know yet how to prevent those stubs
 *
 *  so, to find dlopen find address range dlopen is in in this process (/proc/selfpid/maps), get the base address of that specific module
 *  and then add that offset to the same named module in the target process
 *
 */

#include <stdlib.h>

#include <stdio.h>
#include <sys/wait.h>

#include <sys/ptrace.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#ifdef HAS_LINUX_USER_H
#include <linux/user.h>
#else
#include <sys/user.h>
#endif

#include <dlfcn.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <libgen.h>

#ifdef __aarch64__
#include <elf.h>
#ifdef __ANDROID__
#include <arm-linux-androideabi/asm/ptrace.h>
#endif
#endif

#include "symbols.h"
#include "porthelp.h"
#include "api.h"
#include "ceserver.h"

#ifndef SUN_LEN //missing in android (copy from linux sys/un.h)

/* Evaluate to actual length of the `sockaddr_un' structure.  */
# define SUN_LEN(ptr) ((size_t) (((struct sockaddr_un *) 0)->sun_path)        \
          + strlen ((ptr)->sun_path))
#endif



int WaitForPid(int *status)
{
  int pid=-1;
  while (pid==-1)
  {
    pid=waitpid(-1, status, __WALL);
    if ((pid==-1) && (errno!=EINTR))
    {
      debug_log("LoadExtension wait fail. :%d\n", errno);
      return -1; //something bad happened
    }
  }
  return pid;
}

int showRegisters(int pid)
{
  /*
#ifdef __aarch64__
  struct user_pt_regs regs;
#else
  #ifdef __arm__
    struct pt_regs r;
  #else
    struct user_regs_struct r;
  #endif
#endif




  int result=ptrace(PTRACE_GETREGS, pid, 0, &r);



  if (result!=0)
  {
    debug_log("PTRACE_GETREGS FAILED (%d)\n", result);
    return result;
  }

#ifdef __arm__
  debug_log("r0=%lx\n", r.ARM_r0);
  debug_log("orig_r0=%lx\n", r.ARM_ORIG_r0);
  debug_log("pc=%lx\n", r.ARM_pc);
#else
  #if defined(__x86_64__)
    debug_log("RAX=%lx\n", r.rax);
    debug_log("orig_rax=%lx\n", r.orig_rax);
    debug_log("rip=%lx\n", r.rip);
  #endif

  #if defined(__i386__)
    debug_log("EAX=%lx\n", r.eax);
    debug_log("orig_eax=%lx\n", r.orig_eax);
    debug_log("eip=%lx\n", r.eip);
  #endif
#endif
*/

    return 0;
}

uintptr_t finddlopen(int pid, uintptr_t *_dlerror)  //todo: use the elf parsing routines (they work better now)
{
    void *libdl;
    void *realdlopen;
    void *realdlerror;
    libdl=dlopen("libdl.so", RTLD_NOW);

    debug_log("libdl=%p\n", libdl);

    realdlopen=dlsym(libdl,"dlopen");
    realdlerror=dlsym(libdl,"dlerror");
    debug_log("dlopen=%p\n", dlopen);
    debug_log("realdlopen=%p\n", realdlopen);

    debug_log("dlerror=%p\n", dlerror);
    debug_log("realdlerror=%p\n", realdlerror);
#ifndef __arm__
    if (dlopen==realdlopen)
      debug_log("Please tell db what you did to get this to function (excluding manually editing this if statement)\n");
#endif


    //open /proc/self/maps and look up the region that holds realdlopen

    FILE *maps=fopen("/proc/self/maps", "r");

    char x[200];
    char currentmodule[256];
    char modulepath[256];

    unsigned long long currentmodulestart;

    currentmodule[0]=0;


    while (fgets(x, 200, maps))
    {
      unsigned long long start;
      unsigned long long stop;
      debug_log("%s", x);

      sscanf(x, "%llx-%llx %*s %*s %*s %*s %s\n", &start, &stop, modulepath);

      if (strcmp(modulepath, currentmodule)!=0)
      {
         strcpy(currentmodule, modulepath);
         currentmodulestart=start;
      }

      if (
           (((uintptr_t)realdlopen)>=start) &&
           (((uintptr_t)realdlopen)<stop)
         )
      {
        unsigned int offset=(uintptr_t)realdlopen-currentmodulestart;
        unsigned int offset2=(uintptr_t)realdlerror-currentmodulestart;
        char mapsfilename[255];
        debug_log("found it. Module: %s Offset=%x\n", currentmodule, offset);

        //find this module in the target process and apply this offset to get the address of dlopen
        sprintf(mapsfilename, "/proc/%d/maps", pid);
        FILE *maps2=fopen(mapsfilename, "r");
        if (maps2)
        {
          char y[200];
          while (fgets(y, 200, maps2))
          {
             if (y[strlen(y)-1]!='\n')
             {
               //need to go to the end of line first

               char discard[100];

               do
               {
                 discard[99]=0;
                 fgets(discard, 99, maps);
               } while (discard[99]!=0);
             }


             debug_log("%s", y);

             modulepath[0]='\0';
             sscanf(y, "%llx-%llx %*s %*s %*s %*s %s\n", &start, &stop, modulepath);

             debug_log("Check if '%s' == '%s'\n", modulepath, currentmodule);
             if (strcmp(modulepath, currentmodule)==0)
             {
                debug_log("found the module in the target process\n");
                fclose(maps);
                fclose(maps2);
                *_dlerror=start+offset2;
                return start+offset;
             }
          }
          fclose(maps2);

        }
        else
        {
           debug_log("Failure to open %s\n", mapsfilename);
        }


        fclose(maps);
        return 0;
      }
      else debug_log("Nope\n");

    }

    fclose(maps);

    return 1;
}

void writeString(int pid, uintptr_t address, char *string)
{
  int l=strlen(string)+1;
  long *p;
  long v;
  int i;
  int bs;
  i=0;

  debug_log("l=%d\n", l);


  while (i<l)
  {
    p=(long *)&string[i];
    if ((l-i)<sizeof(long))
    {
      bs=sizeof(long);
      v=*p;
    }
    else
    {
      v=string[i];
      bs=1;
    }

    safe_ptrace(PTRACE_POKEDATA, pid, (void*)(address+i), (void*)v);
    i+=bs;
  }
}

int openExtension(int pid, int *openedSocket)
{
  int i;
  int s;
  int al;
  char name[256];
  s=socket(AF_UNIX, SOCK_STREAM, 0);
  debug_log("s=%d\n", s);

  sprintf(name, " ceserver_extension%d", pid);

  struct sockaddr_un address;
  address.sun_family=AF_UNIX;
  strcpy(address.sun_path, name);

  al=SUN_LEN(&address);

  address.sun_path[0]=0;
  i=connect(s, (struct sockaddr *)&address, al);

  if (i==0)
  {
    debug_log("Successful connection\n");
    *openedSocket=s;
    return 1;
  }
  else
  {
    close(s);
    return 0;
  }
}

int isExtensionLoaded(int pid)
{
  int s;
  int result=openExtension(pid, &s);

  if (result)
    close(s);

  return result;
}

int loadExtension(PProcessData p, char *path)
{

    int pid;
    uintptr_t dlerror;
    uintptr_t str;
    int status;
    int pathlen=strlen(path)+1; //0-terminater

    debug_log("loadExtension()\n");

    debug_log("Phase 0: Check if it's already open\n");
    if (isExtensionLoaded(p->pid))
    {
      debug_log("Already loaded\n");
      return TRUE;
    }
    else
      debug_log("Not yet loaded\n");





    if (p->dlopen==0) //fallback to the old method
    {
      debug_log("Phase 1: Find dlopen in target\n");
      p->dlopen=finddlopen(p->pid, &dlerror);
    }

    if (p->dlopen==0)
    {
      debug_log("dlopen==NULL Abort!\n");
      return 0;
    }

    debug_log("dlopen=%p\n", (void *)p->dlopen);
    //debug_log("dlerror=%p\n", (void *)dlerror);


    if (!p->isDebugged)
    {
      pid=ptrace_attach_andwait(p->pid);

      debug_log("After wait. PID=%d\n", pid);
      //safe_ptrace(PTRACE_CONT,pid,0,0);
    }
    else
    {
      debug_log("Killing pid %d\n", p->pid);

      int e=kill(p->pid, SIGSTOP);
      debug_log("kill returned %d\n", e);

      debug_log("Waiting for thread to stop\n");
      pid=WaitForPid(&status);

      if (WIFSTOPPED(status))
        debug_log("Stopped with signal %d\n", WSTOPSIG(status));
      else
        debug_log("Unexpected status: %x\n", status);

    }

    showRegisters(pid);



printf("After wait 2. PID=%d\n", pid);




    //save the current state and set the state to what I need it to be
#ifdef __i386__
  struct user_regs_struct origregs;
  struct user_regs_struct newregs;
#endif

#ifdef __x86_64__
  struct user_regs_struct origregs;
  struct user_regs_struct newregs;
#endif


#ifdef __arm__
  struct pt_regs origregs;
  struct pt_regs newregs;
#endif


#ifdef __aarch64__
  struct user_pt_regs origregs;
  struct user_pt_regs newregs;

  typedef struct _pt_regs32 {
    uint32_t uregs[18];
  } pt_regs32, *ppt_regs32;
  pt_regs32 origregs32;
  pt_regs32 newregs32;
  struct iovec iov;
#endif

#ifdef __aarch64__
      if (p->is64bit)
      {
        iov.iov_base=&newregs;
        iov.iov_len=sizeof(newregs);
      }
      else
      {
        iov.iov_base=&newregs32;
        iov.iov_len=sizeof(newregs32);
      }

      if (ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov))
#else
      if (ptrace(PTRACE_GETREGS, pid, 0, &newregs)!=0)
#endif
      {
        debug_log("PTRACE_GETREGS FAILED\n");
        safe_ptrace(PTRACE_DETACH, pid,0,0);

        return FALSE;
      }

#ifdef __aarch64__
      if (p->is64bit)
      {
        iov.iov_base=&origregs;
        iov.iov_len=sizeof(origregs);
      }
      else
      {
        iov.iov_base=&origregs32;
        iov.iov_len=sizeof(origregs32);
      }
      if (ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov))
#else
      if (ptrace(PTRACE_GETREGS, pid, 0, &origregs)!=0)
#endif
      {
        debug_log("PTRACE_GETREGS FAILED 2\n");
        safe_ptrace(PTRACE_DETACH, pid,0,0);

        return FALSE;
      }



      uintptr_t returnaddress=0x0ce0;


#ifdef __arm__
      //allocate space in the stack

      newregs.ARM_sp-=8+4*((pathlen+3)/ 4);

      //not sur eif [sp] is written to with a push or if it's [sp-4] and then sp decreased, so start at sp+4 instead
      str=newregs.ARM_sp+4;
      writeString(pid, str, path);

      newregs.ARM_lr=returnaddress;
      newregs.ARM_pc=p->dlopen;
      newregs.ARM_r0=str;
      newregs.ARM_r1=RTLD_NOW;
      newregs.ARM_r2=p->dlopencaller; //needed by android: loader_dlopen


      if (newregs.ARM_pc & 1)
      {
         //THUMB Address link
         debug_log("THUMB destination\n");
         newregs.ARM_cpsr=newregs.ARM_cpsr | (1 << 5);

         //not sure how to set the J bit (thumbee uses it...)
         //for now disable it until a bug happens
         newregs.ARM_cpsr=newregs.ARM_cpsr & (~(1<<25)); //unset J


      }
      else
      {
        debug_log("ARM destination\n");
        debug_log("newregs.ARM_cpsr was %x\n", newregs.ARM_cpsr);
        newregs.ARM_cpsr=newregs.ARM_cpsr & (~(1<<5)); //unset T
        newregs.ARM_cpsr=newregs.ARM_cpsr & (~(1<<25)); //unset J
        debug_log("newregs.ARM_cpsr is %x\n", newregs.ARM_cpsr);
      }

      debug_log("r0=%lx\n", origregs.ARM_r0);
      debug_log("orig_r0=%lx\n", origregs.ARM_ORIG_r0);
      debug_log("pc=%lx\n", origregs.ARM_pc);
      debug_log("cpsr=%lx\n", origregs.ARM_cpsr);

#endif

#ifdef __aarch64__
      //todo: if target is 32-bit .....
      if (p->is64bit==0)
      {
        debug_log("orig pc=%lx\n", origregs32.ARM_pc);
        debug_log("orig sp=%lx\n", origregs32.ARM_sp);
        debug_log("orig cpsr=%lx\n", origregs32.ARM_cpsr);

        newregs32.ARM_sp-=8+4*((pathlen+3)/ 4);

        //not sur eif [sp] is written to with a push or if it's [sp-4] and then sp decreased, so start at sp+4 instead
        str=newregs32.ARM_sp+4;
        writeString(pid, str, path);

        newregs32.ARM_lr=returnaddress;
        newregs32.ARM_pc=p->dlopen;
        newregs32.ARM_r0=str;
        newregs32.ARM_r1=RTLD_NOW;
        newregs32.ARM_r2=p->dlopencaller; //needed by android: loader_dlopen



        if (newregs32.ARM_pc & 1)
        {
           //THUMB Address link
           debug_log("THUMB destination\n");
           newregs32.ARM_cpsr=newregs32.ARM_cpsr | (1 << 5);

           //not sure how to set the J bit (thumbee uses it...)
           //for now disable it until a bug happens
           newregs32.ARM_cpsr=newregs32.ARM_cpsr & (~(1<<25)); //unset J


        }
        else
        {
          debug_log("ARM destination\n");
          debug_log("newregs32.ARM_cpsr was %x\n", newregs32.ARM_cpsr);
          newregs32.ARM_cpsr=newregs32.ARM_cpsr & (~(1<<5)); //unset T
          newregs32.ARM_cpsr=newregs32.ARM_cpsr & (~(1<<25)); //unset J
          debug_log("newregs32.ARM_cpsr is %x\n", newregs32.ARM_cpsr);
        }

        debug_log("new pc=%lx\n", newregs32.ARM_pc);
        debug_log("new sp=%lx\n", newregs32.ARM_sp);
        debug_log("new cpsr=%lx\n", newregs32.ARM_cpsr);


      }
      else
      {
        debug_log("64-bit target\n");

        debug_log("orig pc=%llx\n", origregs.pc);
        debug_log("orig sp=%llx\n", origregs.sp);
        debug_log("orig lr=%llx\n", origregs.regs[30]);
        debug_log("orig x0=%llx\n", origregs.regs[0]);
        debug_log("orig x1=%llx\n", origregs.regs[1]);



        //allocate space in the stack

        newregs.sp-=16+16*((pathlen+3)/16);
        str=newregs.sp;
        writeString(pid, str, path);

        debug_log("injecting in aarch64\n");

        newregs.regs[30]=returnaddress;  //30=LR
        newregs.pc=p->dlopen;
        newregs.regs[0]=str;
        newregs.regs[1]=RTLD_NOW;
        newregs.regs[2]=p->dlopencaller; //needed by android: loader_dlopen

        debug_log("new pc=%llx\n", origregs.pc);
        debug_log("new sp=%llx\n", origregs.sp);
        debug_log("new lr=%llx\n", origregs.regs[30]);
        debug_log("new x0=%llx\n", origregs.regs[0]);
        debug_log("new x1=%llx\n", origregs.regs[1]);
      }
#endif

#ifdef __x86_64__
      debug_log("rax=%lx\n", origregs.rax);
      debug_log("rbp=%lx\n", origregs.rbp);
      debug_log("rsp=%lx\n", origregs.rsp);
      debug_log("orig_rax=%lx\n", origregs.orig_rax);
      debug_log("rip=%lx\n", origregs.rip);



      //allocate stackspace
      newregs.rsp=newregs.rsp-0x28-(8*((pathlen+7) / 8));

      //check that the first 4 bits of rsp are 1000 (8) (aligned with the function return push)
      if ((newregs.rsp & 0xf)!=8)
      {
        debug_log("Aligning stack.  Was %llx", newregs.rsp);
        newregs.rsp-=8;
        newregs.rsp&=~(0xf); //clear the first 4 bits

        newregs.rsp=newregs.rsp | 8; //set to 8

        debug_log(" is now %llx\n", newregs.rsp);
      }
      //set the return address

      debug_log("Writing 0x0ce0 to %lx\n", newregs.rsp);
      if (ptrace(PTRACE_POKEDATA, pid, newregs.rsp, returnaddress)!=0)
      {
        debug_log("Failed to write return address\n");
        safe_ptrace(PTRACE_DETACH, pid,0,0);

        return FALSE;
      }

      if (ptrace(PTRACE_POKEDATA, pid, newregs.rsp-8, returnaddress)!=0)
      {
        debug_log("Fuck\n");
        safe_ptrace(PTRACE_DETACH, pid,0,0);

        return FALSE;
      }

      if (ptrace(PTRACE_POKEDATA, pid, newregs.rsp+8, returnaddress)!=0)
      {
        debug_log("Fuck\n");
        safe_ptrace(PTRACE_DETACH, pid,0,0);

        return FALSE;
      }


     //write the path at rsp+10

     str=newregs.rsp+0x18;
     writeString(pid, str, path);

     debug_log("str=%p\n", (void *)str);



     returnaddress=ptrace(PTRACE_PEEKDATA, pid, newregs.rsp, 0);
     debug_log("[%lx]=%lx", newregs.rsp, returnaddress);


      newregs.rip=p->dlopen;
      newregs.rax=0;
      newregs.rdi=str;
      newregs.rsi=RTLD_NOW;
      newregs.rdx=p->dlopencaller;
      newregs.orig_rax=0;
#endif

#ifdef __i386__
    debug_log("eax=%lx\n", origregs.eax);
    debug_log("ebp=%lx\n", origregs.ebp);
    debug_log("esp=%lx\n", origregs.esp);
    debug_log("orig_eax=%lx\n", origregs.orig_eax);
    debug_log("eip=%lx\n", origregs.eip);

    //allocate stackspace
    newregs.esp=newregs.esp-0x28-(8*((pathlen+7) / 8));
    if ((newregs.esp & 0xf)!=8)
    {
      debug_log("Aligning stack.  Was %llx", newregs.esp);
      newregs.esp-=8;
      newregs.esp&=~(0xf); //clear the first 4 bits

      newregs.esp=newregs.esp | 8; //set to 8

      debug_log(" is now %llx\n", newregs.esp);
    }

    //in 32-bit the stack will have to look like:
    //0-3: Return address  (0x0ce0)
    //4-7: Address to path
    //8-11:RTLD_NOW
    //12-...: Path

    //


    if (ptrace(PTRACE_POKEDATA, pid, newregs.esp+0, returnaddress)!=0)
    {
      debug_log("Fuck\n");
      safe_ptrace(PTRACE_DETACH, pid,0,0);

      return FALSE;
    }

    if (ptrace(PTRACE_POKEDATA, pid, newregs.esp+4, newregs.esp+12)!=0)
    {
      debug_log("Fuck2\n");
      safe_ptrace(PTRACE_DETACH, pid,0,0);

      return FALSE;
    }

    if (ptrace(PTRACE_POKEDATA, pid, newregs.esp+8, RTLD_NOW)!=0)
    {
      debug_log("Fuck3\n");
      safe_ptrace(PTRACE_DETACH, pid,0,0);

      return FALSE;
    }

    writeString(pid, newregs.esp+12, path);

    newregs.eip=p->dlopen;
    newregs.orig_eax=0;
#endif

#ifdef __aarch64__
    if (p->is64bit)
    {
      iov.iov_base=&newregs;
      iov.iov_len=sizeof(newregs);
    }
    else
    {
      iov.iov_base=&newregs32;
      iov.iov_len=sizeof(newregs32);
    }
      if (ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov))
#else
      if (ptrace(PTRACE_SETREGS, pid, 0, &newregs)!=0)
#endif
      {
        debug_log("PTRACE_SETREGS FAILED\n");
        safe_ptrace(PTRACE_DETACH, pid,0,0);

        return FALSE;
      }

#ifdef __aarch64__
     if (p->is64bit)
     {
       iov.iov_base=&newregs;
       iov.iov_len=sizeof(newregs);
     }
     else
     {
       iov.iov_base=&newregs32;
       iov.iov_len=sizeof(newregs32);
     }
     if (ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov))
#else
     if (ptrace(PTRACE_GETREGS, pid, 0, &newregs)!=0)
#endif
     {
       debug_log("PTRACE_GETREGS FAILED 4\n");
       safe_ptrace(PTRACE_DETACH, pid,0,0);

       return FALSE;
     }

     debug_log("after setregs:\n");

#ifdef __arm__
     debug_log("r0=%lx\n", newregs.ARM_r0);
     debug_log("orig_r0=%lx\n", newregs.ARM_ORIG_r0);
     debug_log("pc=%lx\n", newregs.ARM_pc);
     debug_log("cpsr=%lx\n", newregs.ARM_cpsr);
#endif

#ifdef __aarch64__
     debug_log("pc=%llx\n", newregs.pc);
     debug_log("sp=%llx\n", newregs.sp);
     debug_log("lr=%llx\n", newregs.regs[30]);
     debug_log("x0=%llx\n", newregs.regs[0]);
     debug_log("x1=%llx\n", newregs.regs[1]);
#endif

#ifdef __x86_64__
     debug_log("rax=%lx\n", newregs.rax);
     debug_log("rdi=%lx\n", newregs.rdi);
     debug_log("rsi=%lx\n", newregs.rsi);
     debug_log("rbp=%lx\n", newregs.rbp);
     debug_log("rsp=%lx\n", newregs.rsp);
     debug_log("orig_rax=%lx\n", newregs.orig_rax);
     debug_log("rip=%lx\n", newregs.rip);
#endif

#ifdef __i386__
     debug_log("eax=%lx\n", newregs.eax);
     debug_log("edi=%lx\n", newregs.edi);
     debug_log("esi=%lx\n", newregs.esi);
     debug_log("ebp=%lx\n", newregs.ebp);
     debug_log("esp=%lx\n", newregs.esp);
     debug_log("orig_eax=%lx\n", newregs.orig_eax);
     debug_log("eip=%lx\n", newregs.eip);
#endif //__x86_64__

    debug_log("\n\nContinuing thread\n");


    int ptr;
    ptr=ptrace(PTRACE_CONT,pid,(void *)0,(void *)SIGCONT);

    debug_log("PRACE_CONT=%d\n", ptr);
    if (ptr!=0)
    {
      debug_log("PTRACE_CONT FAILED\n");
      return 1;
    }

    //wait for this thread to crash
    int pid2;

    pid2=-1;
    while (pid2==-1)
    {
      pid2=waitpid(-1, &status,  WUNTRACED| __WALL);

      if (WIFSTOPPED(status))
      {
        debug_log("Stopped with signal %d\n", WSTOPSIG(status));

        if (pid2!=pid)
        {
          debug_log("It's a different thread\n");
          if (!p->isDebugged)
          {
            debug_log("No debugger present. Continuing it unhandled\n");

            if (WSTOPSIG(status)!=SIGSTOP)
              ptrace(PTRACE_CONT,pid2,(void *)0,(void *)(uintptr_t)WSTOPSIG(status));
            else
              ptrace(PTRACE_CONT,pid2,(void *)0,0);
          }
          else
          {
            //add it to the debug events
            DebugEvent de;
            de.threadid=pid2;
            de.debugevent=WSTOPSIG(status);
            AddDebugEventToQueue(p, &de);

            debug_log("Debugger present. Added to the queue\n");
          }
          pid2=-1;
          continue;
        }

      }
      else
        debug_log("Unexpected status: %x\n", status);


      if ((pid2==-1) && (errno!=EINTR))
      {
        debug_log("LoadExtension wait fail. :%d\n", errno);

        return FALSE;
      }

      if (pid2==0)
        pid2=-1;



      debug_log(".");
    }

    debug_log("after wait: pid=%d (status=%x)\n", pid, status);

    siginfo_t si;
    if (ptrace(PTRACE_GETSIGINFO, pid, NULL, &si)!=0)
    {
      debug_log("GETSIGINFO FAILED\n");
      safe_ptrace(PTRACE_DETACH, pid,0,0);
      return FALSE;
    }

    debug_log("si.si_signo=%d\n", si.si_signo);

//    if (si.si_signo==SIGSEGV)
      //debug_log("si._sifields._sigfault._addr=%x\n", si._sifields._sigfault._addr);




#ifdef __aarch64__
      if (p->is64bit)
      {
        iov.iov_base=&newregs;
        iov.iov_len=sizeof(newregs);
      }
      else
      {
        iov.iov_base=&newregs32;
        iov.iov_len=sizeof(newregs32);
      }
      if (ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov))
#else
     if (ptrace(PTRACE_GETREGS, pid, 0, &newregs)!=0)
#endif
     {
       debug_log("PTRACE_GETREGS FAILED (2)\n");
       safe_ptrace(PTRACE_DETACH, pid,0,0);

       return FALSE;
     }

#ifdef __arm__
    debug_log("r0=%lx\n", newregs.ARM_r0);
    debug_log("orig_r0=%lx\n", newregs.ARM_ORIG_r0);
    debug_log("pc=%lx\n", newregs.ARM_pc);
    debug_log("sp=%lx\n", newregs.ARM_sp);
    debug_log("cpsr=%lx\n", newregs.ARM_cpsr);
#endif

#ifdef __aarch64__
    if (p->is64bit)
    {
      debug_log("pc=%llx\n", newregs.pc);
      debug_log("sp=%llx\n", newregs.sp);
      debug_log("lr=%llx\n", newregs.regs[30]);
      debug_log("x0=%llx\n", newregs.regs[0]);
      debug_log("x1=%llx\n", newregs.regs[1]);
    }
    else
    {
      debug_log("r0=%lx\n", newregs32.ARM_r0);
      debug_log("orig_r0=%lx\n", newregs32.ARM_ORIG_r0);
      debug_log("pc=%lx\n", newregs32.ARM_pc);
      debug_log("sp=%lx\n", newregs32.ARM_sp);
      debug_log("cpsr=%lx\n", newregs32.ARM_cpsr);
    }

#endif

#ifdef __x86_64__
    debug_log("rax=%lx\n", newregs.rax);
    debug_log("rdi=%lx\n", newregs.rdi);
    debug_log("rsi=%lx\n", newregs.rsi);
    debug_log("rbp=%lx\n", newregs.rbp);
    debug_log("rsp=%lx\n", newregs.rsp);
    debug_log("orig_rax=%lx\n", newregs.rax);
    debug_log("rip=%lx\n", newregs.rip);
#endif

#ifdef __i386__
     debug_log("eax=%lx\n", newregs.eax);
     debug_log("edi=%lx\n", newregs.edi);
     debug_log("esi=%lx\n", newregs.esi);
     debug_log("ebp=%lx\n", newregs.ebp);
     debug_log("esp=%lx\n", newregs.esp);
     debug_log("orig_eax=%lx\n", newregs.eax);
     debug_log("eip=%lx\n", newregs.eip);
#endif


#ifdef __aarch64__
     if (p->is64bit)
     {
       iov.iov_base=&origregs;
       iov.iov_len=sizeof(origregs);
     }
     else
     {
       iov.iov_base=&origregs32;
       iov.iov_len=sizeof(origregs32);
     }
     if (ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov))
#else
     if (ptrace(PTRACE_SETREGS, pid, 0, &origregs)!=0)
#endif
     {
       debug_log("PTRACE_SETREGS FAILED (20\n");
     }

     if (!p->isDebugged)
     {
       debug_log("Detaching\n");
       if (ptrace(PTRACE_DETACH, pid,0,0)!=0)
         debug_log("PTRACE_DETACH FAILED\n");
     }
     else
     {
       if (ptrace(PTRACE_CONT,pid,(void *)0,(void *)SIGCONT)!=0)
         debug_log("PTRACE_CONT failed\n");
     }


     debug_log("End...\n");

     return 1;

}

void finddlopencallback(uintptr_t address, char *symbolname, PProcessData context)
{
  debug_log("found dlopen at %llx\n", address);
  context->dlopen=address;

}

int loadCEServerExtension(HANDLE hProcess)
{
  debug_log("loadCEServerExtension\n");
  if (GetHandleType(hProcess) == htProcesHandle )
  {
    PProcessData p=(PProcessData)GetPointerFromHandle(hProcess);


    if (p->isDebugged)
    {
      debug_log("this process is being debugged\n");
      //make sure this is executed by the debugger thread
      if (p->debuggerThreadID!=pthread_self())
      {
        debug_log("Not the debugger thread. Switching...\n");
        //tell the debugger thread to do this
        int result=0;
#pragma pack(1)
        struct
        {
          uint8_t command;
          uint32_t pHandle;
        } lx;
#pragma pack()

        lx.command=CMD_LOADEXTENSION;
        lx.pHandle=hProcess;
        if (pthread_mutex_lock(&debugsocketmutex) == 0)
        {
          sendall(p->debuggerClient, &lx, sizeof(lx), 0);
          WakeDebuggerThread();

          recvall(p->debuggerClient, &result, sizeof(result), MSG_WAITALL);
          debug_log("Returned from debugger thread. Result:%d\n", result);

          pthread_mutex_unlock(&debugsocketmutex);
        }

        return result;
      }
      else
        debug_log("This is the debugger thread\n");
    }



    if (p->hasLoadedExtension==0)
    {
      char modulepath[256], modulepath2[256];
      int l;

      memset(modulepath, 0, 256);
      memset(modulepath2, 0, 256);

      char *mp;


      l=readlink("/proc/self/exe", modulepath2, 256);

      if (l!=-1)
      {
        modulepath2[l]=0;
        debug_log("modulepath2=%s\n", modulepath2);
        sscanf(modulepath2,"%s", modulepath); //sometimes it has a (deleted) text after it

        debug_log("modulepath=%s\n", modulepath);
        mp=dirname(modulepath);

        debug_log("after dirname: %s\n", mp);
        strcpy(modulepath, mp);
        strcat(modulepath, "/libceserver-extension");

#ifdef __i386__
        strcat(modulepath, "_x86");
#endif

#ifdef __x86_64__
        if (p->is64bit)
          strcat(modulepath, "_x86_64");
        else
          strcat(modulepath, "_x86");
#endif

#ifdef __aarch64__
        if (p->is64bit)
          strcat(modulepath, "_arm64");
        else
          strcat(modulepath, "_arm");
#endif

#ifdef __arm__
        strcat(modulepath, "_arm");
#endif
        strcat(modulepath,".so");



      }
      else
      {
        strcpy(modulepath, "libceserver-extension");

#ifdef __i386__
        strcat(modulepath, "_x86");
#endif

#ifdef __x86_64__
        if (p->is64bit)
          strcat(modulepath, "_x86_64");
        else
          strcat(modulepath, "_x86");
#endif

#ifdef __aarch64__
        if (p->is64bit)
          strcat(modulepath, "_arm64");
        else
          strcat(modulepath, "_arm");
#endif

#ifdef __arm__
        strcat(modulepath, "_arm");
#endif
        strcat(modulepath,".so");
      }

      debug_log("modulepath = %s\n", modulepath);


      if (p->isDebugged)
      {
        debug_log("This process is being debugged. Checking if it's already loaded\n");

        pthread_mutex_lock(&p->extensionMutex);
        p->hasLoadedExtension=openExtension(p->pid, &p->extensionFD);
        pthread_mutex_unlock(&p->extensionMutex);
      }
     // else

      if (p->hasLoadedExtension)
        debug_log("The extension is already loaded\n");

      debug_log("Scanning for dlopen\n");

#ifdef __ANDROID__
      debug_log("Trying to find __loader_dlopen\n");
      FindSymbol(hProcess,"__loader_dlopen", (symcallback)finddlopencallback, p);

      if (p->dlopen)
        debug_log("__loader_dlopen at %p\n", p->dlopen);
      else
        debug_log("__loader_dlopen not found\n");

      if (p->dlopencaller==0)
      {
        //find the first system module base address
        debug_log("trying to find a suitable caller origin\n");
        HANDLE ths;
        ModuleListEntry me;
        ths=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,p->pid);

        if (Module32First(ths, &me)) do
        {
          if (strncmp(me.moduleName,"/system/bin/",12)==0)
          {
            debug_log("found: ");
            debug_log(me.moduleName);
            debug_log("\n");
            p->dlopencaller=me.baseAddress+0x1000;
            break;
          }
        } while (Module32Next(ths,&me));

        if ((p->dlopencaller==0) && (Module32First(ths, &me))) do
        {
          debug_log("no /system/bin, trying system\n");
          if (strncmp(me.moduleName,"/system/bin/",12)==0)
          {
            debug_log("found: ");
            debug_log(me.moduleName);
            debug_log("\n");
            p->dlopencaller=me.baseAddress+0x1000;
            break;
          }
        } while (Module32Next(ths,&me));

        if ((p->dlopencaller==0) && (Module32First(ths, &me)))
        {
          debug_log("no /system. fuck it! picking the first module I see: \n");
          debug_log("found: ");
          debug_log(me.moduleName);
          debug_log("\n");
          p->dlopencaller=me.baseAddress+0x1000;
        }

        CloseHandle(ths);

      }
#endif

      if (p->dlopen==0)
      {
        debug_log("Trying to find dlopen\n");
        FindSymbol(hProcess,"dlopen", (symcallback)finddlopencallback, p);

        if (p->dlopen)
          debug_log("dlopen at %p\n", p->dlopen);
        else
          debug_log("dlopen not found\n");
      }

      if (p->dlopen==0)
        debug_log("failure finding dlopen\n");

      {
        pthread_mutex_lock(&p->extensionMutex);
        if (p->hasLoadedExtension==0) //still 0
        {

          if (p->neverForceLoadExtension==0)
          {
            debug_log("Calling loadExtension\n");
            p->hasLoadedExtension=loadExtension(p, modulepath);

          }

          if (p->hasLoadedExtension)
            p->hasLoadedExtension=openExtension(p->pid, &p->extensionFD);


          debug_log("p->hasLoadedExtension=%d\n", p->hasLoadedExtension);
        }

        pthread_mutex_unlock(&p->extensionMutex);
      }


    }
    else
      debug_log("Already loaded\n");

    return p->hasLoadedExtension;
  }
  else
  {
    debug_log("Invalid handle type");
    return 0;
  }
}
