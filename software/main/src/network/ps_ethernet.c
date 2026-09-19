#include "ps_ethernet.h"
#include "../protocol/nclp_text_command.h"
#include "nclp_udp_packetizer.h"
#include "../protocol/nclp_command_service.h"

#include "xparameters.h"
#include "xemacps.h"
#include "xiicps.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xiltimer.h"
#include "sleep.h"

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "netif/xadapter.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define NCLP_GEM1_BASEADDR       0xFF0C0000U
#define NCLP_BANK1_CTRL5         0xFF180164U
#define NCLP_GEM1_MIO_PULL_MASK  0x00003FFFU
#define NCLP_GEM1_MIO_PULL_VALUE 0x0000357FU
#define NCLP_KR260_PHY_ADDR      8U
#define NCLP_RESET_I2C_ADDR      0x11U
#define NCLP_RESET_REG           0xDBU
#define NCLP_GEM1_RESET_MASK     0x40U
#define NCLP_TCP_RX_BYTES        512U
#define NCLP_TCP_TX_BYTES        256U

static const char g_sfp_owner_rejection[] =
    "NCLP CONTROLLED BY SFP LINK\r\n";

typedef struct {
    struct tcp_pcb *pcb;
    uint32_t connection_generation;
    uint16_t rx_used;
    uint16_t reply_bytes;
    uint8_t connected;
    uint8_t reply_buffer_pending;
    uint8_t command_reply_pending;
    uint8_t command_submit_in_progress;
    uint8_t rx_buffer[NCLP_TCP_RX_BYTES];
    uint8_t reply_buffer[NCLP_TCP_TX_BYTES];
} nclp_tcp_control_client_t;

extern XEmacPs_Config XEmacPs_ConfigTable[];

static struct netif g_netif;
static struct tcp_pcb *g_tcp_listener;
static struct udp_pcb *g_udp_stream;
static uint32_t g_stream_destination_ipv4 = 0xC0A8000AU;
static uint16_t g_stream_destination_port = NCLP_DATA_PORT;
static uint32_t g_tcp_connection_generation;
static uint32_t g_ethernet_application_enabled = 1U;
static nclp_tcp_control_client_t g_tcp_client;
static XTime g_fast_timer;
static XTime g_slow_timer;
static XTime g_link_timer;

static void process_tcp_rx_buffer(void);
static void detach_tcp_callbacks(struct tcp_pcb *pcb);
static err_t close_tcp_pcb_or_abort(struct tcp_pcb *pcb);

static void clear_tcp_client_state(nclp_tcp_control_client_t *client)
{
    client->connected = 0U;
    client->pcb = NULL;
    client->rx_used = 0U;
    client->reply_buffer_pending = 0U;
    client->reply_bytes = 0U;
    client->command_reply_pending = 0U;
    client->command_submit_in_progress = 0U;
    client->connection_generation++;
}

static err_t reject_tcp_connection_for_sfp(struct tcp_pcb *pcb)
{
    const uint16_t bytes = (uint16_t)(sizeof(g_sfp_owner_rejection) - 1U);

    if (pcb == NULL) {
        return ERR_OK;
    }
    if (tcp_sndbuf(pcb) >= bytes &&
        tcp_write(pcb, g_sfp_owner_rejection, bytes,
                  TCP_WRITE_FLAG_COPY) == ERR_OK) {
        (void)tcp_output(pcb);
    }
    detach_tcp_callbacks(pcb);
    return close_tcp_pcb_or_abort(pcb);
}

static void detach_tcp_callbacks(struct tcp_pcb *pcb)
{
    if (pcb == NULL) {
        return;
    }
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_poll(pcb, NULL, 0U);
}

static err_t close_tcp_pcb_or_abort(struct tcp_pcb *pcb)
{
    if (pcb != NULL && tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static void retire_tcp_client(void)
{
    struct tcp_pcb *pcb;

    if (g_tcp_client.connected == 0U) {
        return;
    }
    pcb = g_tcp_client.pcb;
    clear_tcp_client_state(&g_tcp_client);
    if (pcb != NULL) {
        detach_tcp_callbacks(pcb);
        (void)close_tcp_pcb_or_abort(pcb);
    }
}

static uint32_t read_le32(const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

static void write_le32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

static int kr260_configure_gem1_mio_straps(void)
{
    uint32_t value = Xil_In32(NCLP_BANK1_CTRL5);

    value = (value & ~NCLP_GEM1_MIO_PULL_MASK) |
            NCLP_GEM1_MIO_PULL_VALUE;
    Xil_Out32(NCLP_BANK1_CTRL5, value);
    value = Xil_In32(NCLP_BANK1_CTRL5);
    if ((value & NCLP_GEM1_MIO_PULL_MASK) != NCLP_GEM1_MIO_PULL_VALUE) {
        xil_printf("  FAIL KR260 GEM1 MIO straps  BANK1_CTRL5=%08lx\r\n",
                   (unsigned long)value);
        return -1;
    }
    xil_printf("  PASS KR260 GEM1 MIO straps  BANK1_CTRL5=%08lx\r\n",
               (unsigned long)value);
    return 0;
}

static int i2c_read_reg(XIicPs *iic, uint8_t device, uint8_t reg,
                        uint8_t *value)
{
    int status;

    XIicPs_SetOptions(iic, XIICPS_REP_START_OPTION);
    status = XIicPs_MasterSendPolled(iic, &reg, 1U, device);
    if (status == XST_SUCCESS) {
        status = XIicPs_MasterRecvPolled(iic, value, 1U, device);
    }
    XIicPs_ClearOptions(iic, XIICPS_REP_START_OPTION);
    while (XIicPs_BusIsBusy(iic)) {
    }
    return status;
}

static int i2c_write_reg(XIicPs *iic, uint8_t device, uint8_t reg,
                         uint8_t value)
{
    uint8_t message[2] = {reg, value};
    int status = XIicPs_MasterSendPolled(iic, message, sizeof(message), device);

    while (XIicPs_BusIsBusy(iic)) {
    }
    return status;
}

static int kr260_release_gem1_phy(void)
{
    XIicPs iic;
    XIicPs_Config *config;
    uint8_t outputs;

    config = XIicPs_LookupConfig((UINTPTR)XPAR_XIICPS_0_BASEADDR);
    if (config == NULL ||
        XIicPs_CfgInitialize(&iic, config,
                             (uint32_t)config->BaseAddress) !=
            XST_SUCCESS ||
        XIicPs_SetSClk(&iic, 100000U) != XST_SUCCESS) {
        xil_printf("  FAIL KR260 I2C1 initialization\r\n");
        return -1;
    }
    if (i2c_read_reg(&iic, NCLP_RESET_I2C_ADDR, NCLP_RESET_REG,
                     &outputs) != XST_SUCCESS) {
        xil_printf("  FAIL reset expander read addr=0x11 reg=0xDB\r\n");
        return -1;
    }
    outputs &= (uint8_t)~NCLP_GEM1_RESET_MASK;
    if (i2c_write_reg(&iic, NCLP_RESET_I2C_ADDR, NCLP_RESET_REG,
                      outputs) != XST_SUCCESS) {
        xil_printf("  FAIL assert GEM1 PHY reset\r\n");
        return -1;
    }
    usleep(100U);
    outputs |= NCLP_GEM1_RESET_MASK;
    if (i2c_write_reg(&iic, NCLP_RESET_I2C_ADDR, NCLP_RESET_REG,
                      outputs) != XST_SUCCESS) {
        xil_printf("  FAIL release GEM1 PHY reset\r\n");
        return -1;
    }
    usleep(1000U);
    xil_printf("  PASS KR260 GEM1 PHY reset    I2C1 0x11 bit6 released\r\n");
    return 0;
}

static int select_kr260_gem1_phy(void)
{
    uint32_t index;

    for (index = 0U; XEmacPs_ConfigTable[index].Name != NULL; ++index) {
        if (XEmacPs_ConfigTable[index].BaseAddress == NCLP_GEM1_BASEADDR) {
            XEmacPs_ConfigTable[index].PhyAddr = NCLP_KR260_PHY_ADDR;
            XEmacPs_ConfigTable[index].MdioProducerBaseAddr =
                NCLP_GEM1_BASEADDR;
            xil_printf("  INFO AMD lwIP PHY selection  PHY=8 MDIO=GEM1\r\n");
            return 0;
        }
    }
    xil_printf("  FAIL GEM1 configuration table entry not found\r\n");
    return -1;
}

static void service_tcp_reply_buffer(void)
{
    err_t error;

    if (g_tcp_client.connected == 0U || g_tcp_client.pcb == NULL ||
        g_tcp_client.reply_buffer_pending == 0U ||
        tcp_sndbuf(g_tcp_client.pcb) < g_tcp_client.reply_bytes) {
        return;
    }
    error = tcp_write(g_tcp_client.pcb, g_tcp_client.reply_buffer,
                      g_tcp_client.reply_bytes, TCP_WRITE_FLAG_COPY);
    if (error == ERR_OK) {
        /* lwIP owns a copy now.  A tcp_output error must not cause a retry,
         * because the bytes can already be queued and would be duplicated. */
        g_tcp_client.reply_buffer_pending = 0U;
        g_tcp_client.reply_bytes = 0U;
        (void)tcp_output(g_tcp_client.pcb);
    }
}

static int tcp_send_bytes(struct tcp_pcb *pcb, const void *data,
                          uint16_t bytes)
{
    if (pcb == NULL || data == NULL || bytes == 0U) {
        return -1;
    }
    if (g_tcp_client.connected != 0U && pcb == g_tcp_client.pcb) {
        if (g_tcp_client.reply_buffer_pending != 0U ||
            bytes > sizeof(g_tcp_client.reply_buffer)) {
            return -1;
        }
        memcpy(g_tcp_client.reply_buffer, data, bytes);
        g_tcp_client.reply_bytes = bytes;
        g_tcp_client.reply_buffer_pending = 1U;
        service_tcp_reply_buffer();
        return 0;
    }
    if (tcp_sndbuf(pcb) < bytes ||
        tcp_write(pcb, data, bytes, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        return -1;
    }
    (void)tcp_output(pcb);
    return 0;
}

static int tcp_send_text(struct tcp_pcb *pcb, const char *text)
{
    return tcp_send_bytes(pcb, text, (uint16_t)strlen(text));
}

static void encode_binary_reply(const nclp_main_reply_t *reply,
                                uint8_t bytes[NCLP_REPLY_WORDS * 4U])
{
    const uint32_t words[NCLP_REPLY_WORDS] = {
        reply->magic, reply->version, reply->command, reply->sequence,
        reply->status, reply->data0, reply->data1, reply->data2,
        reply->data3, reply->fail_count
    };

    for (uint32_t i = 0U; i < NCLP_REPLY_WORDS; ++i) {
        write_le32(&bytes[i * 4U], words[i]);
    }
}

static int decode_binary_command(
    const uint8_t bytes[NCLP_CMD_WORDS * 4U],
    nclp_main_command_t *command)
{
    if (read_le32(&bytes[0]) != NCLP_CMD_MAGIC ||
        read_le32(&bytes[4]) != NCLP_CMD_VERSION) {
        return -1;
    }

    command->command = read_le32(&bytes[8]);
    command->sequence = read_le32(&bytes[12]);
    for (uint32_t i = 0U; i < (NCLP_CMD_WORDS - 4U); ++i) {
        command->args[i] = read_le32(&bytes[(i + 4U) * 4U]);
    }
    return 0;
}

static int send_text_reply(struct tcp_pcb *pcb,
                           const nclp_main_reply_t *reply)
{
    char response[192];
    const char *status_name = reply->status == NCLP_COMMAND_STATUS_OK ? "OK" :
                              (reply->status ==
                                       NCLP_COMMAND_STATUS_CANCELLED ?
                                   "CANCELLED" :
                                   (reply->status ==
                                            NCLP_COMMAND_STATUS_NO_CHIP_DETECTED ?
                                        "NO_CHIP" : "ERROR"));

    (void)snprintf(response, sizeof(response),
                   "NCLP %s CMD=0x%02lx STATUS=%lu D0=0x%08lx D1=0x%08lx D2=0x%08lx D3=0x%08lx FAIL=%lu\r\n",
                   status_name,
                   (unsigned long)reply->command,
                   (unsigned long)reply->status,
                   (unsigned long)reply->data0,
                   (unsigned long)reply->data1,
                   (unsigned long)reply->data2,
                   (unsigned long)reply->data3,
                   (unsigned long)reply->fail_count);
    return tcp_send_text(pcb, response);
}

static int send_command_reply(uint32_t connection_generation,
                              uint8_t binary_reply,
                              const nclp_main_reply_t *reply)
{
    if (g_tcp_client.connected == 0U ||
        g_tcp_client.connection_generation != connection_generation) {
        return -1;
    }
    if (binary_reply != 0U) {
        uint8_t output[NCLP_REPLY_WORDS * 4U];
        encode_binary_reply(reply, output);
        return tcp_send_bytes(g_tcp_client.pcb, output, sizeof(output));
    }
    return send_text_reply(g_tcp_client.pcb, reply);
}

static int queue_tcp_command_reply(uint32_t token, const nclp_main_reply_t *reply)
{
    uint32_t connection_generation = token >> 1U;
    int result;

    if (g_tcp_client.connected == 0U ||
        g_tcp_client.connection_generation != connection_generation) {
        return 0; /* The old connection owns no output on a new connection. */
    }
    result = send_command_reply(connection_generation,
                                (uint8_t)(token & 1U), reply);
    if (result == 0 && g_tcp_client.command_submit_in_progress == 0U) {
        g_tcp_client.command_reply_pending = 0U;
    }
    return result;
}

static int dispatch_tcp_command(const nclp_main_command_t *command,
                                uint32_t connection_generation,
                                uint8_t binary_reply)
{
    int result;

    if (command->command == NCLP_CMD_SET_UDP_DEST) {
        nclp_main_reply_t reply = {0};
        uint32_t port = command->args[1] == 0U ?
                            NCLP_DATA_PORT : command->args[1];
        reply.magic = NCLP_CMD_MAGIC;
        reply.version = NCLP_CMD_VERSION;
        reply.command = command->command;
        reply.sequence = command->sequence;
        if (nclp_ps_ethernet_set_udp_destination(command->args[0], port) != 0) {
            reply.status = NCLP_COMMAND_STATUS_NETWORK;
        }
        reply.data0 = g_stream_destination_ipv4;
        reply.data1 = g_stream_destination_port;
        (void)send_command_reply(connection_generation, binary_reply, &reply);
        return 0;
    }
    g_tcp_client.command_submit_in_progress = 1U;
    result = nclp_command_service_submit(
        command, queue_tcp_command_reply,
        (connection_generation << 1U) | binary_reply);
    g_tcp_client.command_submit_in_progress = 0U;
    return result;
}

static void consume_tcp_rx_bytes(uint16_t count)
{
    if (count >= g_tcp_client.rx_used) {
        g_tcp_client.rx_used = 0U;
        return;
    }
    memmove(g_tcp_client.rx_buffer, g_tcp_client.rx_buffer + count,
            g_tcp_client.rx_used - count);
    g_tcp_client.rx_used = (uint16_t)(g_tcp_client.rx_used - count);
}

static void process_tcp_rx_buffer(void)
{
    static const char command_error[] =
        "NCLP ERROR invalid command or arguments\r\n";

    while (g_tcp_client.connected != 0U && g_tcp_client.rx_used != 0U &&
           g_tcp_client.reply_buffer_pending == 0U) {
        nclp_main_command_t command;
        uint8_t binary_command = 0U;
        uint16_t frame_bytes = 0U;

        if (g_tcp_client.rx_used >= 4U &&
            read_le32(g_tcp_client.rx_buffer) == NCLP_CMD_MAGIC) {
            frame_bytes = NCLP_CMD_WORDS * 4U;
            if (g_tcp_client.rx_used < frame_bytes) {
                return;
            }
            binary_command = 1U;
            if (decode_binary_command(g_tcp_client.rx_buffer, &command) != 0) {
                consume_tcp_rx_bytes(frame_bytes);
                (void)tcp_send_text(g_tcp_client.pcb, command_error);
                continue;
            }
        } else {
            uint16_t newline = 0U;
            char line[192];
            int found = 0;

            for (newline = 0U; newline < g_tcp_client.rx_used; ++newline) {
                if (g_tcp_client.rx_buffer[newline] == '\n') {
                    found = 1;
                    break;
                }
            }
            if (found == 0) {
                return;
            }
            frame_bytes = (uint16_t)(newline + 1U);
            if (newline >= sizeof(line)) {
                consume_tcp_rx_bytes(frame_bytes);
                (void)tcp_send_text(g_tcp_client.pcb, command_error);
                continue;
            }
            memcpy(line, g_tcp_client.rx_buffer, newline);
            line[newline] = '\0';
            while (newline > 0U &&
                   (line[newline - 1U] == '\r' ||
                    line[newline - 1U] == ' ')) {
                line[--newline] = '\0';
            }
            if (nclp_command_parse_text(line, &command) != 0) {
                consume_tcp_rx_bytes(frame_bytes);
                (void)tcp_send_text(g_tcp_client.pcb, command_error);
                continue;
            }
            xil_printf("  INFO TCP command received     %s\r\n", line);
        }

        consume_tcp_rx_bytes(frame_bytes);
        if (dispatch_tcp_command(&command,
                                 g_tcp_client.connection_generation,
                                 binary_command) > 0) {
            g_tcp_client.command_reply_pending = 1U;
        }
    }
}

static void tcp_client_error(void *arg, err_t error)
{
    nclp_tcp_control_client_t *client = (nclp_tcp_control_client_t *)arg;
    (void)error;

    if (client == &g_tcp_client) {
        clear_tcp_client_state(client);
    }
}

static err_t tcp_receive(void *arg, struct tcp_pcb *pcb,
                         struct pbuf *packet, err_t error)
{
    nclp_tcp_control_client_t *client = (nclp_tcp_control_client_t *)arg;

    if (packet == NULL) {
        if (client == &g_tcp_client && client->pcb == pcb) {
            clear_tcp_client_state(client);
        }
        detach_tcp_callbacks(pcb);
        return close_tcp_pcb_or_abort(pcb);
    }
    if (error != ERR_OK) {
        pbuf_free(packet);
        return error;
    }
    if (client != &g_tcp_client || client->pcb != pcb ||
        client->connected == 0U) {
        tcp_recved(pcb, packet->tot_len);
        pbuf_free(packet);
        detach_tcp_callbacks(pcb);
        return close_tcp_pcb_or_abort(pcb);
    }

    if (g_ethernet_application_enabled == 0U) {
        tcp_recved(pcb, packet->tot_len);
        pbuf_free(packet);
        /* The previous owner must still deliver an accepted command's reply.
         * Drop later RX bytes until that reply has entered lwIP; the transport
         * selection state machine retires the connection immediately after. */
        client->rx_used = 0U;
        if (client->command_reply_pending != 0U ||
            client->reply_buffer_pending != 0U ||
            client->command_submit_in_progress != 0U) {
            return ERR_OK;
        }
        clear_tcp_client_state(client);
        return reject_tcp_connection_for_sfp(pcb);
    }

    if (packet->tot_len >
        (uint16_t)(sizeof(client->rx_buffer) - client->rx_used)) {
        tcp_recved(pcb, packet->tot_len);
        pbuf_free(packet);
        xil_printf("  WARN TCP control RX overflow; closing client to resynchronize\r\n");
        if (client == &g_tcp_client && client->pcb == pcb) {
            clear_tcp_client_state(client);
        }
        detach_tcp_callbacks(pcb);
        return close_tcp_pcb_or_abort(pcb);
    }
    pbuf_copy_partial(packet, client->rx_buffer + client->rx_used,
                      packet->tot_len, 0U);
    client->rx_used = (uint16_t)(client->rx_used + packet->tot_len);
    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);
    process_tcp_rx_buffer();
    return ERR_OK;
}

static err_t tcp_reply_progress(void *arg, struct tcp_pcb *pcb,
                                uint16_t acknowledged)
{
    nclp_tcp_control_client_t *client = (nclp_tcp_control_client_t *)arg;

    (void)acknowledged;
    if (client == &g_tcp_client && client->pcb == pcb &&
        client->connected != 0U) {
        service_tcp_reply_buffer();
        if (g_ethernet_application_enabled != 0U &&
            client->reply_buffer_pending == 0U) {
            process_tcp_rx_buffer();
        }
    }
    return ERR_OK;
}

static err_t tcp_client_poll(void *arg, struct tcp_pcb *pcb)
{
    return tcp_reply_progress(arg, pcb, 0U);
}

static err_t tcp_accept_connection(void *arg, struct tcp_pcb *pcb, err_t error)
{
    static const char banner[] =
        "NCLP READY TCP4000: commands/config/results; UDP5000 samples; GET_CONFIG; GET_PROGRESS; GET_RESULT type id index\r\n";

    (void)arg;
    if (error != ERR_OK) {
        return error;
    }
    if (g_ethernet_application_enabled == 0U) {
        return reject_tcp_connection_for_sfp(pcb);
    }
    if (g_tcp_client.connected != 0U) {
        (void)tcp_send_text(pcb, "NCLP BUSY control client already connected\r\n");
        detach_tcp_callbacks(pcb);
        return close_tcp_pcb_or_abort(pcb);
    }

    memset(&g_tcp_client, 0, sizeof(g_tcp_client));
    g_tcp_client.connected = 1U;
    g_tcp_client.pcb = pcb;
    g_tcp_client.connection_generation = ++g_tcp_connection_generation;
    tcp_arg(pcb, &g_tcp_client);
    tcp_recv(pcb, tcp_receive);
    tcp_err(pcb, tcp_client_error);
    tcp_sent(pcb, tcp_reply_progress);
    tcp_poll(pcb, tcp_client_poll, 2U);
    tcp_nagle_disable(pcb);
    (void)tcp_send_text(pcb, banner);
    return ERR_OK;
}

void nclp_ps_ethernet_poll(void)
{
    XTime now;

    XTime_GetTime(&now);
    /* AMD's bare-metal lwIP adapter does not create the FreeRTOS
     * link_detect_thread.  Service the same driver state machine here so a
     * PHY that timed out with no cable at boot can negotiate when the cable
     * is attached later, and so a dropped link can recover without resetting
     * the MAC or rebuilding the lwIP interface. */
    if ((now - g_link_timer) >= COUNTS_PER_SECOND) {
        eth_link_detect(&g_netif);
        g_link_timer = now;
    }
    xemacif_input(&g_netif);
    if ((now - g_fast_timer) >= (COUNTS_PER_SECOND / 4U)) {
        tcp_fasttmr();
        g_fast_timer = now;
    }
    if ((now - g_slow_timer) >= (COUNTS_PER_SECOND / 2U)) {
        tcp_slowtmr();
        g_slow_timer = now;
    }
    service_tcp_reply_buffer();
    if (g_ethernet_application_enabled != 0U &&
        g_tcp_client.reply_buffer_pending == 0U) {
        process_tcp_rx_buffer();
    }
    if (g_ethernet_application_enabled == 0U &&
        g_tcp_client.command_reply_pending == 0U &&
        g_tcp_client.reply_buffer_pending == 0U) {
        retire_tcp_client();
    }
}

void nclp_ps_ethernet_set_application_enabled(uint32_t enabled)
{
    enabled = enabled != 0U ? 1U : 0U;
    if (enabled == g_ethernet_application_enabled) {
        return;
    }
    g_ethernet_application_enabled = enabled;
    if (enabled == 0U) {
        /* Never execute bytes received before or during transport takeover. */
        g_tcp_client.rx_used = 0U;
    }
}

int nclp_ps_ethernet_control_idle(void)
{
    return g_tcp_client.command_reply_pending == 0U &&
           g_tcp_client.reply_buffer_pending == 0U;
}

int nclp_ps_ethernet_force_retire_control(void)
{
    if (g_ethernet_application_enabled != 0U ||
        g_tcp_client.command_reply_pending != 0U ||
        g_tcp_client.command_submit_in_progress != 0U) {
        return -1;
    }
    retire_tcp_client();
    return 0;
}

int nclp_ps_ethernet_init(void)
{
    ip_addr_t local_ip;
    ip_addr_t netmask;
    ip_addr_t gateway;
    uint8_t mac[6] = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U};
    err_t error;

    g_ethernet_application_enabled = 1U;
    xil_printf("\r\n[3] KR260 J10C PS GEM1 control/data\r\n");
    if (kr260_configure_gem1_mio_straps() != 0 ||
        kr260_release_gem1_phy() != 0 ||
        select_kr260_gem1_phy() != 0) {
        return -1;
    }

    IP4_ADDR(ip_2_ip4(&local_ip), 192, 168, 0, 42);
    IP4_ADDR(ip_2_ip4(&netmask), 255, 255, 255, 0);
    IP4_ADDR(ip_2_ip4(&gateway), 192, 168, 0, 1);

    lwip_init();
    if (xemac_add(&g_netif, &local_ip, &netmask, &gateway, mac,
                  NCLP_GEM1_BASEADDR) == NULL) {
        xil_printf("  FAIL AMD xemac_add\r\n");
        return -1;
    }
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);

    g_tcp_listener = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (g_tcp_listener == NULL ||
        tcp_bind(g_tcp_listener, IP_ANY_TYPE, NCLP_CONTROL_PORT) != ERR_OK) {
        xil_printf("  FAIL TCP control bind\r\n");
        return -1;
    }
    g_tcp_listener = tcp_listen(g_tcp_listener);
    if (g_tcp_listener == NULL) {
        xil_printf("  FAIL TCP listen\r\n");
        return -1;
    }
    tcp_accept(g_tcp_listener, tcp_accept_connection);

    g_udp_stream = udp_new_ip_type(IPADDR_TYPE_V4);
    if (g_udp_stream == NULL) {
        xil_printf("  FAIL UDP stream allocation\r\n");
        return -1;
    }
    error = (err_t)nclp_ps_ethernet_set_udp_destination(
        g_stream_destination_ipv4, g_stream_destination_port);
    if (error != 0) {
        xil_printf("  FAIL UDP stream destination setup\r\n");
        return -1;
    }

    XTime_GetTime(&g_fast_timer);
    g_slow_timer = g_fast_timer;
    g_link_timer = g_fast_timer;
    /* If PHY autonegotiation timed out inside xemac_add, the adapter records
     * ETH_LINK_DOWN but still constructs a usable netif.  This first service
     * marks lwIP link-down/negotiating; periodic poll calls complete recovery
     * after a cable is connected. */
    eth_link_detect(&g_netif);
    xil_printf("  PASS PS GEM1 interface        192.168.0.42/24 PHY=8\r\n");
    xil_printf("  INFO Ethernet link            hot-plug monitor active\r\n");
    xil_printf("  PASS controls ready           TCP 192.168.0.42:4000 only\r\n");
    xil_printf("  PASS sample format            UDP v%u fixed_bytes=%u\r\n",
               NCLP_UDP_VERSION, NCLP_UDP_FIXED_DATAGRAM_BYTES);
    return 0;
}

int nclp_ps_ethernet_set_udp_destination(uint32_t ipv4, uint32_t port)
{
    ip_addr_t destination;
    err_t error;

    if (g_udp_stream == NULL || ipv4 == 0U || ipv4 == 0xFFFFFFFFU ||
        port == 0U || port > 65535U) {
        return -1;
    }

    IP4_ADDR(ip_2_ip4(&destination),
             (uint8_t)(ipv4 >> 24), (uint8_t)(ipv4 >> 16),
             (uint8_t)(ipv4 >> 8), (uint8_t)ipv4);
    udp_disconnect(g_udp_stream);
    error = udp_connect(g_udp_stream, &destination, (uint16_t)port);
    if (error != ERR_OK) {
        return -1;
    }
    g_stream_destination_ipv4 = ipv4;
    g_stream_destination_port = (uint16_t)port;
    xil_printf("  PASS UDP destination          %lu.%lu.%lu.%lu:%lu\r\n",
               (unsigned long)((ipv4 >> 24) & 0xFFU),
               (unsigned long)((ipv4 >> 16) & 0xFFU),
               (unsigned long)((ipv4 >> 8) & 0xFFU),
               (unsigned long)(ipv4 & 0xFFU),
               (unsigned long)port);
    return 0;
}

int nclp_ps_ethernet_send_datagram(const void *data, uint16_t bytes)
{
    struct pbuf *packet;
    int result = -1;

    if (g_ethernet_application_enabled == 0U || data == NULL ||
        bytes != NCLP_UDP_FIXED_DATAGRAM_BYTES ||
        g_udp_stream == NULL) {
        return -1;
    }

    packet = pbuf_alloc(PBUF_TRANSPORT, bytes, PBUF_RAM);
    if (packet != NULL) {
        if (pbuf_take(packet, data, bytes) == ERR_OK &&
            udp_send(g_udp_stream, packet) == ERR_OK) {
            result = 0;
        }
        pbuf_free(packet);
    }
    return result;
}
