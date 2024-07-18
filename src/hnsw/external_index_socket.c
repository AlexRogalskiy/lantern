#include <postgres.h>

#include "external_index_socket.h"

#include <arpa/inet.h>
#include <hnsw/build.h>
#include <miscadmin.h>
#include <sys/socket.h>
#include <unistd.h>

static bool is_little_endian()
{
    int i = 1;

    return *((char *)&i) == 1;
}

static void set_read_timeout(uint32 client_fd, uint32 seconds)
{
    struct timeval timeout;

    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;

    if(setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) < 0) {
        elog(ERROR, "external index: failed to set receive timeout for socket");
    }
}

static void set_write_timeout(uint32 client_fd, uint32 seconds)
{
    struct timeval timeout;

    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;

    if(setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) < 0) {
        elog(ERROR, "external index: failed to set send timeout for socket");
    }
}

void check_external_index_response_error(uint32 client_fd, unsigned char *buffer, int32 size)
{
    uint32 hdr;
    if(size < 0) {
        close(client_fd);
        elog(ERROR, "external index socket read failed");
    }

    if(size < sizeof(uint32)) return;

    memcpy(&hdr, buffer, sizeof(uint32));

    if(hdr != EXTERNAL_INDEX_ERR_MSG) return;

    // append nullbyte
    buffer[ size ] = '\0';
    close(client_fd);
    elog(ERROR, "external index error: %s", buffer + EXTERNAL_INDEX_MAGIC_MSG_SIZE);
}

void check_external_index_request_error(uint32 client_fd, int32 bytes_written)
{
    if(bytes_written > 0) return;

    close(client_fd);
    elog(ERROR, "external index socket send failed");
}

void external_index_send_codebook(
    uint32 client_fd, float *codebook, uint32 dimensions, uint32 num_centroids, uint32 num_subvectors)
{
    int           data_size = dimensions * sizeof(float);
    int           bytes_written = -1;
    unsigned char buf[ data_size ];

    for(int i = 0; i < num_centroids; i++) {
        memcpy(buf, &codebook[ i * dimensions ], data_size);
        bytes_written = send(client_fd, buf, data_size, 0);
        check_external_index_request_error(client_fd, bytes_written);
    }

    uint32 end_msg = EXTERNAL_INDEX_END_MSG;
    bytes_written = send(client_fd, &end_msg, EXTERNAL_INDEX_MAGIC_MSG_SIZE, 0);

    check_external_index_request_error(client_fd, bytes_written);
}

int create_external_index_session(const char                   *host,
                                  int                           port,
                                  const usearch_init_options_t *params,
                                  const ldb_HnswBuildState     *buildstate,
                                  uint32                        estimated_row_count)
{
    int                client_fd, status;
    unsigned char      init_buf[ sizeof(external_index_params_t) + EXTERNAL_INDEX_MAGIC_MSG_SIZE ];
    struct sockaddr_in serv_addr;
    unsigned char      init_response[ EXTERNAL_INDEX_INIT_BUFFER_SIZE ] = {0};

    if(!is_little_endian()) {
        elog(ERROR, "external indexing is supported only for little endian byte ordering");
    }
    elog(INFO, "connecting to external indexing daemon on %s:%d", host, port);

    if((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        elog(ERROR, "external index: socket creation failed");
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if(inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
        elog(ERROR, "external index: invalid address");
    }

    set_write_timeout(client_fd, EXTERNAL_INDEX_SOCKET_TIMEOUT);
    set_read_timeout(client_fd, EXTERNAL_INDEX_SOCKET_TIMEOUT);

    // TODO:: connect timeout
    if((status = connect(client_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr))) < 0) {
        elog(ERROR, "external index: connection with server failed");
    }

    external_index_params_t index_params = {
        .pq = params->pq,
        .metric_kind = params->metric_kind,
        .quantization = params->quantization,
        .dim = params->dimensions,
        .m = params->connectivity,
        .ef_construction = params->expansion_add,
        .ef = params->expansion_search,
        .num_centroids = params->num_centroids,
        .num_subvectors = params->num_subvectors,
        .estimated_capcity = estimated_row_count,
    };

    uint32 hdr_msg = EXTERNAL_INDEX_INIT_MSG;
    memcpy(init_buf, &hdr_msg, EXTERNAL_INDEX_MAGIC_MSG_SIZE);
    memcpy(init_buf + EXTERNAL_INDEX_MAGIC_MSG_SIZE, &index_params, sizeof(external_index_params_t));
    uint32 bytes_written
        = send(client_fd, init_buf, sizeof(external_index_params_t) + EXTERNAL_INDEX_MAGIC_MSG_SIZE, 0);

    check_external_index_request_error(client_fd, bytes_written);

    if(params->pq) {
        external_index_send_codebook(
            client_fd, buildstate->pq_codebook, params->dimensions, params->num_centroids, params->num_subvectors);
    }

    uint32 buf_size = read(client_fd, &init_response, EXTERNAL_INDEX_INIT_BUFFER_SIZE);

    check_external_index_response_error(client_fd, init_response, buf_size);

    return client_fd;
}

void external_index_receive_index_file(uint32 external_client_fd, uint64 *num_added_vectors, char **result_buf)
{
    uint32        end_msg = EXTERNAL_INDEX_END_MSG;
    unsigned char buffer[ sizeof(uint64_t) ];
    int32         bytes_read, bytes_written;
    uint64        index_size = 0, total_received = 0;

    // disable read timeout while indexing is in progress
    set_read_timeout(external_client_fd, 0);
    // send message indicating that we have finished streaming tuples
    bytes_written = send(external_client_fd, &end_msg, EXTERNAL_INDEX_MAGIC_MSG_SIZE, 0);
    check_external_index_request_error(external_client_fd, bytes_written);

    // read how many tuples have been indexed
    bytes_read = read(external_client_fd, buffer, sizeof(uint64));
    check_external_index_response_error(external_client_fd, buffer, bytes_read);
    memcpy(num_added_vectors, buffer, sizeof(uint64));

    // read index file size
    bytes_read = read(external_client_fd, buffer, sizeof(uint64));
    check_external_index_response_error(external_client_fd, buffer, bytes_read);
    memcpy(&index_size, buffer, sizeof(uint64));

    *result_buf = palloc0(index_size);

    if(*result_buf == NULL) {
        elog(ERROR, "external index: failed to allocate buffer for index file");
    }

    set_read_timeout(external_client_fd, EXTERNAL_INDEX_SOCKET_TIMEOUT);
    // start reading index into buffer
    while(total_received < index_size) {
        bytes_read = read(external_client_fd, *result_buf + total_received, EXTERNAL_INDEX_FILE_BUFFER_SIZE);

        // Using try/catch to close the socket on interrupt
        PG_TRY();
        {
            // Check for CTRL-C interrupts
            CHECK_FOR_INTERRUPTS();
        }
        PG_CATCH();
        {
            close(external_client_fd);
            PG_RE_THROW();
        }
        PG_END_TRY();

        check_external_index_response_error(
            external_client_fd, (unsigned char *)*result_buf + total_received, bytes_read);

        if(bytes_read == 0) {
            break;
        }

        total_received += bytes_read;
    }
}

void external_index_send_tuple(
    uint32 external_client_fd, usearch_label_t *label, void *vector, uint8 scalar_bits, uint32 dimensions)
{
    unsigned char tuple[ EXTERNAL_INDEX_MAX_TUPLE_SIZE ];
    uint32        tuple_size, bytes_written;
    // send tuple over socket if this is external indexing
    tuple_size = sizeof(usearch_label_t) + dimensions * (scalar_bits / 8);
    memcpy(tuple, label, sizeof(usearch_label_t));
    memcpy(tuple + sizeof(usearch_label_t), vector, tuple_size - sizeof(usearch_label_t));
    bytes_written = send(external_client_fd, tuple, tuple_size, 0);
    check_external_index_request_error(external_client_fd, bytes_written);
}