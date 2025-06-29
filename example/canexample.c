// This is a simple example program using can2040 and the PICO SDK.
//
// See the CMakeLists.txt file for information on compiling.

#pragma GCC optimize ("O0") // *********** OPTIMISER LEVEL **************

#include <pico/stdlib.h>
#include <stdio.h>

#include "../src/can2040.h"

// Simple example of irq safe queue (this is not multi-core safe)
#define QUEUE_SIZE 128 // Must be power of 2
static struct {
    uint32_t pull_pos;
    volatile uint32_t push_pos;
    struct can2040_msg queue[QUEUE_SIZE];
} MessageQueue;

// Internal storage for can2040 module
static struct can2040 cbus;

// Main canbus callback (called from irq handler)
static void
can2040_cb(struct can2040 *cd, uint32_t notify, struct can2040_msg *msg)
{
    if (notify == CAN2040_NOTIFY_RX) {
        // Example message filter
        uint32_t id = msg->id;
//        if (id < 0x101 || id > 0x201)
//            return;

        // Add to queue
        uint32_t push_pos = MessageQueue.push_pos;
        uint32_t pull_pos = MessageQueue.pull_pos;
        if (push_pos + 1 == pull_pos)
            // No space in queue
            return;
        MessageQueue.queue[push_pos % QUEUE_SIZE] = *msg;
        MessageQueue.push_pos = push_pos + 1;
    }
}

// PIO interrupt handler
static void
PIOx_IRQHandler(void)
{
    can2040_pio_irq_handler(&cbus);
}

// Initialize the can2040 module
void
canbus_setup(void)
{
    uint32_t pio_num = 0;
    uint32_t sys_clock = SYS_CLK_HZ, bitrate = 125000;
    uint32_t gpio_rx = 4, gpio_tx = 5;

    // Setup canbus
    can2040_setup(&cbus, pio_num);
    can2040_callback_config(&cbus, can2040_cb);

    // Enable irqs
    irq_set_exclusive_handler(PIO0_IRQ_0, PIOx_IRQHandler);
    irq_set_priority(PIO0_IRQ_0, 1);
    irq_set_enabled(PIO0_IRQ_0, 1);

    // Start canbus
    can2040_start(&cbus, sys_clock, bitrate, gpio_rx, gpio_tx);
}

#define LED_PIN 25

int
main(void)
{
    stdio_init_all();
    canbus_setup();

    gpio_init(LED_PIN); // Initialize the LED pin
    gpio_set_dir(LED_PIN, GPIO_OUT); // Set the LED pin as an output

    sleep_ms(1500);
    printf("CAN Bus example running.\n");

    // Main loop
    uint64_t lastTxTrigger = 0;
    uint64_t lastLedFlash = 0;
    for (;;) {
        uint32_t push_pos = MessageQueue.push_pos;
        uint32_t pull_pos = MessageQueue.pull_pos;
        struct can2040_msg msg;
        if (push_pos != pull_pos) {
            // Pop message from local receive queue
            struct can2040_msg *qmsg = &MessageQueue.queue[pull_pos % QUEUE_SIZE];
            msg = *qmsg;
            MessageQueue.pull_pos++;

            // Report message found on local receive queue
            printf("msg: id=0x%x dlc=%d data=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                msg.id, msg.dlc, msg.data[0], msg.data[1], msg.data[2],
                msg.data[3], msg.data[4], msg.data[5], msg.data[6], msg.data[7]);

            bool openLcbFrame = false; 
            if (msg.id & 0x8000000) { openLcbFrame = true; }
            if (openLcbFrame) { 
                printf("Frame Type = OpenLCB Message\n"); 

                uint8_t frameFormat = (uint8_t)((msg.id & 0x07000000) >> 24);
                uint16_t canMTI = (uint16_t)((msg.id & 0x00FFF000) >> 12);
                printf("\tFrame Format = %u and CAN MTI = 0x%04X\n", frameFormat, canMTI); 

                if (frameFormat == 0) {
                    printf("\tFrame Format = RESERVED.\n");
                } else if (frameFormat == 1) {
                    printf("\tGlobal & Addressed MTI Frame Format\n");
                    uint8_t grossPriority = (msg.id & 0x00C00000) >> 22;
                    uint8_t typeWithinPriority = (msg.id & 0x003E0000) >> 17;
                    uint8_t simpleProtocol = (msg.id & 0x00010000) >> 16;
                    uint8_t addressPresent = (msg.id & 0x00008000) >> 15;
                    uint8_t eventPresent = (msg.id & 0x00004000) >> 14;
                    uint8_t messageModifier = (msg.id & 0x00003000) >> 12;
                    uint16_t sourceAddress = msg.id & 0x00000FFF;
                    uint64_t nodeAddress = ((uint64_t)(msg.data[0]) << 40) | ((uint64_t)(msg.data[1]) << 32) |
                                           ((uint64_t)(msg.data[2]) << 24) | ((uint64_t)(msg.data[3]) << 16) | 
                                           ((uint64_t)(msg.data[4]) << 8) | ((uint64_t)(msg.data[5]));

                    printf("\t\tCAN MTI Gross message priority = %u\n", grossPriority); 
                    printf("\t\tCAN MTI Minor priority determination = %u\n", typeWithinPriority); 
                    printf("\t\tCAN MTI 1=should be handled by simple nodes = %u\n", simpleProtocol); 
                    printf("\t\tCAN MTI 1=has a destination address-field = %u\n", addressPresent); 
                    printf("\t\tCAN MTI 1=This message has an event-field = %u\n", eventPresent); 
                    printf("\t\tCAN MTI Message-specific extra information = %u\n", messageModifier); 

                    switch (grossPriority) {
                        case 0x00: switch (typeWithinPriority) {
                            case 0x08: 
                                printf("\t\t\tInitialisation Complete message from 0x%03X, node 0x%012llX\n", sourceAddress, nodeAddress); 
                                break;
                            default: 
                                printf("\t\t\tUndeciphered message.\n"); 
                                break;
                        }; break;
                        case 0x01: switch (typeWithinPriority) {
                            case 0x06:
                                switch (messageModifier) {
                                    case 0: 
                                        printf("\t\t\tConsumer Identified - Valid message.\n"); 
                                        break;
                                    case 1: 
                                        printf("\t\t\tConsumer Identified - Invalid message.\n"); 
                                        break;
                                    case 3:
                                        printf("\t\t\tConsumer Identified - Unknown message.\n"); 
                                        break;
                                    default:
                                        break;
                                } 
                                break;
                            default: 
                                printf("\t\t\tUndeciphered message.\n"); 
                                break;
                        }; 
                        break;
                        default: 
                            printf("\t\t\tUndeciphered message.\n"); 
                            break;
                    }
                } else if (frameFormat == 2) {
                    printf("\tFrame Format = Datagram complete in frame.\n");
                } else if (frameFormat == 3) {
                    printf("\tFrame Format = Datagram first frame.\n");
                } else if (frameFormat == 4) {
                    printf("\tFrame Format = Datagram middle frame.\n");
                } else if (frameFormat == 5) {
                    printf("\tFrame Format = Datagram final frame.\n");
                } else if (frameFormat == 6) {
                    printf("\tFrame Format = RESERVED.\n");
                } else if (frameFormat == 7) {
                    printf("\tFrame Format = Stream Data.\n");
                }
            }
            else { 
                printf("\tFrame Type = CAN Control Frame\n"); 

                uint32_t content = (msg.id & 0x07FFF000) >> 12;
                printf("\tContent = 0x%04X\n", content);
                
                if (content == 0x0700) { printf ("\t\tReserve ID Frame.\n"); }
                else if (content == 0x0701) { printf ("\t\tAlias Map Definition Frame.\n"); }
                else if (content == 0x0702) { printf ("\t\tAlias Mapping Enquiry(AME) Frame.\n"); }
                else if (content == 0x0703) { printf ("\t\tAlias Map Reset (AMR) Frame.\n"); }
                else if (content == 0x0710) { printf ("\t\tError Information Report 0.\n"); }
                else if (content == 0x0711) { printf ("\t\tError Information Report 1.\n"); }
                else if (content == 0x0712) { printf ("\t\tError Information Report 2.\n"); }
                else if (content == 0x0713) { printf ("\t\tError Information Report 3.\n"); }
                else if (content > 0x1000) { printf ("\t\tCheck ID Frame.\n"); }
                else { printf ("\t\tReserved Frame Type - should not have been sent!.\n"); }

                uint32_t frameSeqNum = content >> 12;
                uint32_t sourceNidAlias = (msg.id & 0x00000FFF);
                printf("\t\t\tFrame sequence number = %u\n", frameSeqNum);
                printf("\t\t\tNode ID being checked = 0x%03X\n", content & 0xFFF);
                printf("\t\t\tSource NID Alias = 0x%03X\n", sourceNidAlias);
            }
        }

/*        
        if (time_us_64() - lastTxTrigger > 1000000) {
            lastTxTrigger = time_us_64();
            // Demo of message transmit
//            if (msg.id == 0x101) {
                struct can2040_msg tmsg;
                tmsg.id = 0x102;
                tmsg.dlc = 8;
                tmsg.data32[0] = 0xabcd;
                tmsg.data32[1] = 0x01020304; // msg.data32[0];
                int sts = can2040_transmit(&cbus, &tmsg);
                printf("Sent message (status=%d)\n", sts);
//            }
        }
*/

        if (time_us_64() - lastLedFlash > 250000) {
            lastLedFlash = time_us_64();
            // Invert LED State
            gpio_put(LED_PIN, !gpio_get(LED_PIN));
        }
    }

    return 0;
}
