#ifndef GAMEMUSIC_H
#define GAMEMUSIC_H

// Installs / removes MiniAmp's libpadsp wrapper in OnionOS's miyoo/lib
// (the original is kept as libpadsp_orig.so). Return 1 on success.
int gm_install(void);
int gm_uninstall(void);
int gm_installed(void);

// background service (miniamp --service)
int service_running(void);       // pid, or 0
void service_stop(void);         // asks it to save its place and quit; waits
void service_spawn(void);        // starts it detached
int service_main(void);

#endif
