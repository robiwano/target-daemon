# Target daemon

## Protocol

Simple text based TCP protocol. A program consists of one or more commands separated by semicolon (;). A new program is usually started with **C** to clear out the current program.

### Commands

**C** : Clear program. Removes all previous commands.  
**T***xxx* : Delta time from last entry (*xxx* is in seconds, with decimalpoint if needed)  
**A** *<*path*>* : Start playing the audio file at *<*path*>* (in programs)  
**M** *<*pos*>* : Move target to *<*pos*>* which can be **1** for target facing shooters, or **0** for target turned away (in 
programs).  
**P** *<*path*>* : Start playing the audio file at *<*path*>* directly.  
**D** *<*pos*>* : Directly move target to *<*pos*>*. Returns error if program is currently executing.

**R** : Run current program.  
**S** : Stop currently executing program, will reset program to start.  

**Q** : Queries current state. Can be given as command at any time.

**X** : Exit server process. Can only be done from the active client.

### Generic Responses
**OK** : Message was received and processed OK  
**ERROR**=*<*error*>* : An error occurred, *<*error*>* can be one of:
- *Busy* : Another TCP client is connected to the daemon. Socket is disconnected directly after this message.
- *Executing* : A new program is given during a program execution.
- *Syntax* : Some error in the given message.
- *Empty* : No program given.
- *UnknownCommand* : Command not recognized.
- *TBD* : ...  

#### Query state response

    EXEC=<xx>         # Current execution time. Empty if program is not running.
    PROG=<tt>         # Total program time in seconds. Empty if no program.
    POS=<0|1>         # 1 if target is facing forwards

Example program (Milsnabb 10 s):

    C;T0;M1;A/audio/kalle/ladda.wav
    T50.0;A/audio/kalle/fardiga.wav
    T0;M0
    T10;M1
    T10;M0
    T2;A/audio/kalle/EUH.wav
    T2;A/audio/kalle/funktion.wav
    T5;M1

*Response*

    OK

Start executing program:

    R

*Response*

    OK

Give new program during executing a program results in:

    ERROR=Executing
