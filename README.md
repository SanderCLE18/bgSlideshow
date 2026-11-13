# bgSlideshow

Slideshow program that sets the background on Omarchy distro, (easiliy portable to hyprland though). 

Program features signal catching such that it can be "pkilled" with different signals and act accordingly,(synergizes well with keybindings that sends these signals).
For example has SIGRTMIN been repurposed to make the program pause on the current image, SIGUSR1 wakes up-then-skips, and SIGUSR2 rewinds to the previous image. 

Tried writing it using C++23 standards. 

Sloppy commit history as it's a small personal project that just displays and changes background images. 
