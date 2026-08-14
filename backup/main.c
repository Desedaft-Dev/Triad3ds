#include <3ds.h>          // Must always be first!
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

static u32 *socBuffer = NULL;

//Telnet crap that strips IAC and other bs
void handle_telnet_stream(unsigned char *buffer, int length, int sock) {
    int clean_len = 0;
    for (int i = 0; i < length; i++) {
        if (buffer[i] == 255) { // IAC (Interpret As Command)
            if (i + 2 < length) {
                unsigned char command = buffer[i + 1];
                unsigned char option  = buffer[i + 2];
                unsigned char reply[3];
                reply[0] = 255; // IAC
                
                if (command == 253) {       // DO -> reply WONT
                    reply[1] = 252;
                    reply[2] = option;
                    send(sock, reply, 3, 0);
                } else if (command == 251) { // WILL -> reply DONT
                    reply[1] = 254;
                    reply[2] = option;
                    send(sock, reply, 3, 0);
                }
                i += 2;
                continue;
            }
        }
        buffer[clean_len++] = buffer[i];
    }
    buffer[clean_len] = '\0';
}

//Define some variables and stuffs
int HP = 0;
int MaxHP = 0;
int Energy = 0;
int MaxEnergy = 0;
int Hunger = 0;
int MaxHunger = 0;
int Thirst = 0;
int MaxThirst = 0;

int initialSetup = 0;
//Update bottom screen with de stats
void draw_bottom_hud(PrintConsole *bot_con) {
    consoleSelect(bot_con);

    
    printf("\x1b[1;1H========================================");
    
    
    printf("\x1b[2;1H HP:     %3d/%3d   |   Energy: %3d/%3d      ", 
           HP, MaxHP, Energy, MaxEnergy);
    
    
    printf("\x1b[3;1H----------------------------------------");

  
    printf("\x1b[4;1H Hunger: %3d/%3d   |   Thirst: %3d/%3d      ", 
           Hunger, MaxHunger, Thirst, MaxThirst);
    
   
    printf("\x1b[5;1H========================================");

    printf("\x1b[28;1H [ Tap screen to open keyboard ]        ");
}


#define MAX_HISTORY_LINES 300
#define MAX_COLS 50       
#define CONSOLE_HEIGHT 28 

char history[MAX_HISTORY_LINES][MAX_COLS + 1];
int history_head = 0;     
int history_count = 0;    // Total lines currently stored
int scroll_offset = 0;    // 0 = live bottom view, >0 = scrolled up into history

//Add line into history
void add_to_history(const char *line) {
    //strncpy(history[history_head], line, MAX_COLS);
    snprintf(history[history_head], MAX_COLS + 1, "%s", line);
    history[history_head][MAX_COLS] = '\0';
    
    history_head = (history_head + 1) % MAX_HISTORY_LINES;
    if (history_count < MAX_HISTORY_LINES) {
        history_count++;
    }
}

//Draw top consoe, changes if you are browsing
void redraw_console(PrintConsole *con) {
    consoleSelect(con);
    consoleClear();

    int total = history_count;
    int end_idx = total - scroll_offset; 
    int start_idx = end_idx - CONSOLE_HEIGHT;
    if (start_idx < 0) start_idx = 0;
    if (end_idx > total) end_idx = total;

    for (int i = start_idx; i < end_idx; i++) {
        int idx = (history_head - history_count + i + MAX_HISTORY_LINES) % MAX_HISTORY_LINES;
        printf("%s\n", history[idx]);
    }

    // Show a small indicator status bar if looking back in history
    if (scroll_offset > 0) {
        printf("\x1b[30;1H-- SCROLLED UP (%d) [CirclePad to scroll] --", scroll_offset);
    }
}

//Print line with correct wrapping to avoid being cut off
void print_word_wrapped(const char *text, PrintConsole *con) {
    static char line_buf[MAX_COLS + 1];
    static int line_len = 0;

    for (int i = 0; text[i] != '\0'; i++) {
        char c = text[i];
        if (c == '\r') continue;

        if (c == '\n') {
            line_buf[line_len] = '\0';
            add_to_history(line_buf);
            if (scroll_offset == 0) {
                redraw_console(con);
            }
            line_len = 0;
            continue;
        }

        if (line_len < MAX_COLS) {
            line_buf[line_len++] = c;
            line_buf[line_len] = '\0';
        }

        if (line_len >= MAX_COLS) {
            int break_idx = -1;
            for (int j = line_len - 1; j >= 0; j--) {
                if (line_buf[j] == ' ') {
                    break_idx = j;
                    break;
                }
            }

            if (break_idx != -1) {
                line_buf[break_idx] = '\0';
                add_to_history(line_buf);
                if (scroll_offset == 0) redraw_console(con);
                
                int rem_len = line_len - (break_idx + 1);
                memmove(line_buf, &line_buf[break_idx + 1], rem_len);
                line_len = rem_len;
                line_buf[line_len] = '\0';
            } else {
                add_to_history(line_buf);
                if (scroll_offset == 0) redraw_console(con);
                line_len = 0;
            }
        }
    }
    
    if (line_len > 0) {
        add_to_history(line_buf);
        if (scroll_offset == 0) redraw_console(con);
        line_len = 0;
    }
}



void UpdateStatus(int sock){
    send(sock, "health\r\n", strlen("health\r\n"), 0);
    send(sock, "energy\r\n", strlen("energy\r\n"), 0);
    send(sock, "hunger\r\n", strlen("hunger\r\n"), 0);
    send(sock, "thirst\r\n", strlen("thirst\r\n"), 0);
}

//Main loop, duh
int main(int argc, char* argv[]) {
    gfxInitDefault();
    PrintConsole topScreenConsole;
    PrintConsole bottomScreenConsole;
    consoleInit(GFX_TOP, &topScreenConsole);
    consoleInit(GFX_BOTTOM, &bottomScreenConsole);

    printf("\x1b[1;1HInitializing 3DS network...");

    socBuffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!socBuffer) {
        printf("\x1b[3;1HError: Out of memory for socket buffer.");
        goto main_exit;
    }

    if (socInit(socBuffer, SOC_BUFFERSIZE) != 0) {
        printf("\x1b[3;1HError: socInit failed.");
        goto main_exit;
    }

    printf("\x1b[2;1HResolving TriadCity server...");

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo("triadcity.smartmonsters.com", "9094", &hints, &res) != 0) {
        printf("\x1b[3;1HError: Failed to resolve hostname.");
        goto net_exit;
    }
    //ai does not stand for AI, stands for addrinfo
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        printf("\x1b[3;1HError: Could not create socket.");
        freeaddrinfo(res);
        goto net_exit;
    }

    printf("\x1b[3;1HConnecting to TriadCity...");
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        printf("\x1b[4;1HError: Connection failed.");
        close(sock);
        freeaddrinfo(res);
        goto net_exit;
    }

    freeaddrinfo(res); // Clean up address info structure

    // Set socket to non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    consoleSelect(&topScreenConsole);
    consoleClear();
    printf("--- Connected to TriadCity MUD ---\n");
    printf("Press START to exit to Homebrew Launcher.\n\n");

    char rx_buffer[1024];
    char text_input[512];

    circlePosition circlePad;
    static int scroll_cooldown = 0;

    draw_bottom_hud(&bottomScreenConsole);
    u64 lastTime = osGetTime();

    const u64 statusInterval = 10000;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;


        u64 currentTime = osGetTime();

        if(currentTime - lastTime >= statusInterval && initialSetup != 0)
        {
            UpdateStatus(sock);
            lastTime = currentTime;
        }


        hidCircleRead(&circlePad);

        if (scroll_cooldown > 0) scroll_cooldown--;

        // Circle Pad Up (Positive Y) -> Scroll up into history
        if (circlePad.dy > 60 && scroll_cooldown == 0) {
            if (scroll_offset < history_count - CONSOLE_HEIGHT) {
                scroll_offset += 3; // Scroll speed multiplier
                redraw_console(&topScreenConsole);
                scroll_cooldown = 6; // Cooldown frames 
            }
        } 
        // Circle Pad Down (Negative Y) -> Scroll down toward live chat
        else if (circlePad.dy < -60 && scroll_cooldown == 0) {
            if (scroll_offset > 0) {
                scroll_offset -= 3;
                if (scroll_offset < 0) scroll_offset = 0;
                redraw_console(&topScreenConsole);
                scroll_cooldown = 6;
            }
        }

        int bytes_received = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (bytes_received > 0) {
            handle_telnet_stream((unsigned char*)rx_buffer, bytes_received, sock);
            //printf("%s", rx_buffer);

            char *welc_ptr = strstr(rx_buffer, "welcome");
            if(welc_ptr != NULL && initialSetup == 0){
                /*send(sock, "health\r\n", strlen("health\r\n"), 0);
                send(sock, "energy\r\n", strlen("energy\r\n"), 0);
                send(sock, "hunger\r\n", strlen("hunger\r\n"), 0);
                send(sock, "thirst\r\n", strlen("thirst\r\n"), 0);*/
                UpdateStatus(sock);
                initialSetup = 1;
            }


            //Set Health
            char *hp_ptr = strstr(rx_buffer, "Health");
            if(hp_ptr != NULL){
                int current_hp, max_hp;
                if (sscanf(hp_ptr, "Health is currently: %d of %d", &current_hp, &max_hp) == 2) {
                    HP = current_hp;
                    MaxHP = max_hp;
                    draw_bottom_hud(&bottomScreenConsole); // Refresh HUD immediately
                }
            }
            //Set Energy
            char *energy_ptr = strstr(rx_buffer, "Energy");
            if(energy_ptr != NULL){
                int current_energy, max_energy;


                if (sscanf(energy_ptr, "Energy is currently: %d of %d", &current_energy, &max_energy) == 2) {
                    Energy = current_energy;
                    MaxEnergy = max_energy;
                    draw_bottom_hud(&bottomScreenConsole); // Refresh HUD immediately
                }
            }

            //Set Thirst
            char *thirst_ptr = strstr(rx_buffer, "Thirst");
            if(thirst_ptr != NULL){
                int current_thirst, max_thirst;


                if (sscanf(thirst_ptr, "Thirst is currently: %d of %d", &current_thirst, &max_thirst) == 2) {
                    Thirst = current_thirst;
                    MaxThirst = max_thirst;
                    draw_bottom_hud(&bottomScreenConsole); // Refresh HUD immediately
                }
            }

            //Set Hunger
            char *hunger_ptr = strstr(rx_buffer, "Hunger");
            if(hunger_ptr != NULL){
                int current_hunger, max_hunger;


                if (sscanf(hunger_ptr, "Hunger is currently: %d of %d", &current_hunger, &max_hunger) == 2) {
                    Hunger = current_hunger;
                    MaxHunger = max_hunger;
                    draw_bottom_hud(&bottomScreenConsole); // Refresh HUD immediately
                }
            }

            if(hunger_ptr != NULL || thirst_ptr != NULL || energy_ptr != NULL || hp_ptr != NULL){

            }else{
                print_word_wrapped(rx_buffer, &topScreenConsole);
            }
            



        }


       /* if (kDown & KEY_TOUCH) {
                    SwkbdState swkbd;
                    SwkbdButton button = SWKBD_BUTTON_NONE;

                    // Initialize standard software keyboard (2 buttons: Cancel / OK)
                    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
                    swkbdSetHintText(&swkbd, "Enter MUD command...");
                    swkbdSetFeatures(&swkbd, SWKBD_PREDICTIVE_INPUT);

                    // Open keyboard applet (pauses game until user finishes typing)
                    button = swkbdInputText(&swkbd, text_input, sizeof(text_input));

                    // If user clicked OK and typed something
                    if (button == SWKBD_BUTTON_CONFIRM && strlen(text_input) > 0) {
                        // MUD servers typically expect a carriage return/newline at the end of commands
                        strcat(text_input, "\r\n");

                        // Send the saved text variable to the MUD server socket
                        send(sock, text_input, strlen(text_input), 0);

                        printf("\n> %s", text_input); // Echo what you sent to the top screen
                    }
        }*/

        if(kDown)
        {
            switch(kDown){
                case KEY_DDOWN:
                    send(sock, "s\r\n", strlen("s\r\n"), 0);
                break;

                case KEY_DUP:
                    send(sock, "n\r\n", strlen("n\r\n"), 0);
                break;

                case KEY_DLEFT:
                    send(sock, "w\r\n", strlen("w\r\n"), 0);
                break;

                case KEY_DRIGHT:
                    send(sock, "e\r\n", strlen("e\r\n"), 0);
                break;

                case KEY_TOUCH:
                    SwkbdState swkbd;
                        SwkbdButton button = SWKBD_BUTTON_NONE;

                        // Initialize standard software keyboard (2 buttons: Cancel / OK)
                        swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
                        swkbdSetHintText(&swkbd, "Enter MUD command...");
                        //swkbdSetFeatures(&swkbd, SWKBD_PREDICTIVE_INPUT);

                        // Open keyboard applet (pauses game until user finishes typing)
                        button = swkbdInputText(&swkbd, text_input, sizeof(text_input));

                        // If user clicked OK and typed something
                        if (button == SWKBD_BUTTON_CONFIRM && strlen(text_input) > 0) {
                            // MUD servers typically expect a carriage return/newline at the end of commands
                            strcat(text_input, "\r\n");

                            // Send the saved text variable to the MUD server socket
                            send(sock, text_input, strlen(text_input), 0);

                            printf("\n> %s", text_input); // Echo what you sent to the top screen
                        }
                break;


                default://crappy fix to prevent scrolling from filling feed with error message
                    if(kDown != KEY_DOWN && kDown != KEY_LEFT && kDown != KEY_RIGHT && kDown != KEY_UP && kDown != KEY_CPAD_DOWN && kDown != KEY_CPAD_LEFT && kDown != KEY_CPAD_RIGHT && kDown != KEY_CPAD_UP){
                        print_word_wrapped("Unbound Input", &topScreenConsole);
                    }
                    
            }
        }

        

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    close(sock);

net_exit:
    socExit();
    if (socBuffer) free(socBuffer);

main_exit:
    printf("\n\nDisconnected. Press START to exit.");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
    return 0;
}