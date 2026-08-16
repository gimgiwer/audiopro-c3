#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <poll.h>
#include <time.h>

#define SERIAL_PORT "/dev/ttyS0"
#define BAUD_RATE   B57600

static int uart_init(void) {
    int fd = open(SERIAL_PORT, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[-] Error opening %s: %s\n", SERIAL_PORT, strerror(errno));
        return -1;
    }
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) { close(fd); return -1; }
    cfsetospeed(&tty, BAUD_RATE);
    cfsetispeed(&tty, BAUD_RATE);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8 | CLOCAL | CREAD;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG | IEXTEN);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) { close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void uart_send(int fd, const char *cmd) {
    printf("[TX] -> %s", cmd);
    write(fd, cmd, strlen(cmd));
}

int main(void) {
    printf("=== Linkplay Secondary MCU (STM8) Live Diagnostic ===\n");
    int fd = uart_init();
    if (fd < 0) return 1;

    printf("[+] UART %s opened @ 57600 8N1\n", SERIAL_PORT);
    
    // 4-Step Linkplay Handshake
    uart_send(fd, "AXX+MCU+RDY\n");
    usleep(150000);
    uart_send(fd, "AXX+BOT+DON\n");
    usleep(150000);
    uart_send(fd, "AXX+PLM+001\n");
    usleep(150000);
    uart_send(fd, "AXX+MUT+000\n");
    usleep(150000);
    uart_send(fd, "AXX+VOL+035\n");
    usleep(150000);
    uart_send(fd, "AXX+BAT+GET\n");

    printf("\n[*] Listening for MCU RX responses for 5 seconds...\n");
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    time_t start = time(NULL);
    char buf[256];

    while (time(NULL) - start < 5) {
        int ret = poll(&pfd, 1, 500);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                printf("[RX] <- %s", buf);
                fflush(stdout);
            }
        }
    }

    printf("\n[+] Test completed.\n");
    close(fd);
    return 0;
}
