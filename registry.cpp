#include <iostream>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <errno.h>
#include <vector>
#include <string>

// Protocol Action Constants
const uint8_t ACTION_JOIN = 0;
const uint8_t ACTION_PUBLISH = 1;
const uint8_t ACTION_SEARCH = 2;

const int MAX_PEERS = 5;
const int BUFFER_SIZE = 4096;

// Peer entry structure
struct peer_entry  
{
    uint32_t id; // ID of peer
    int socket_descriptor; // Socket descriptor for connection to peer
    bool joined; // flag for checking a peer joining a network
    std::vector<std::string> files; // a list of files in a Peer
    struct sockaddr_in address; // Contains IP address and port number
    uint16_t listen_port; // Port where peer listens for FETCH requests

    // Initialize values for a new class
    peer_entry() : id(0), socket_descriptor(-1), joined(false)
    {
        address.sin_family = AF_INET; // IPv4
        address.sin_addr.s_addr = 0;
        address.sin_port = 0;
    }

    // Reset a status of a class as initialized
    void reset()
    {
        id = 0;
        socket_descriptor = -1;
        joined = false;
        files.clear(); // clear filies in a list
        address.sin_addr.s_addr = 0;
        address.sin_port = 0;
        listen_port = 0;
    }
};

// Global peer table
std::vector<peer_entry> peers(MAX_PEERS); // Hold entries to manage up to 5 peers 

/*
* Set up listening socket
* Return socke fd or -1 for failure
*/
int setup_listen_socket(uint16_t port)
{
    // Make a socket IPv4 and TCP
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt");
        close(sock);
        return -1;
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr)); // Initialize addr by 0
    addr.sin_family = AF_INET; // IPv4
    // INADDR_ANY is to accept any addresses with the same port
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // Host bytes order of any address to Network bytes order ones long
    addr.sin_port = htons(port); // Host bytes order of ports to Network bytes order ones

    // bind info. of IP and port to a socket
    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        perror("bind");
        close(sock);
        return -1;
    }

    // Make a server socket to accept connection request from clients
    // hold up to 5 requests
    if (listen(sock, 5) < 0)
    {
        perror("listen");
        close(sock);
        return -1;
    }

    return sock;
}

/*
* Find an empty slot in the peer registry
* Return index of peers on success, otherwise -1
*/
int find_empty_slot()
{
    for (int i = 0; i < MAX_PEERS; i++)
    {
        if (peers[i].socket_descriptor < 0) // find an empty slot
        {
            return i;
        }
    }

    return -1; // No empty slots
}

/*
* Find peer by socket descriptor
* Return index of a peer on success, otherwise -1
*/
int find_peer_by_socket(int sock)
{
    for (int i = 0; i < MAX_PEERS; i++)
    {
        if (peers[i].socket_descriptor == sock)
        {
            return i;
        }
    }

    return -1; // Not exist a peer
}

/*
* Find a file in the registry
* Return index of a peer on success, otherwise -1 
*/
int find_file(const std::string& filename)
{
    for (int i = 0; i < MAX_PEERS; i++)
    {
        if (peers[i].socket_descriptor >= 0 && peers[i].joined)
        {
            for (const auto& file : peers[i].files)
            {
                if (file == filename)
                {
                    return i; // Find a file
                }
            }
        }
    }

    return -1; // Not exist a file
}

/*
* Remove a peer from the registry
*/
void remove_peer(int index)
{
    if (index >= 0 && index < MAX_PEERS && peers[index].socket_descriptor >= 0)
    {
        close(peers[index].socket_descriptor);
        peers[index].reset();
    }
}

/*
* Handle new connection and add it into peers
*/
void handle_new_connection(int listen_sock)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // Extract one unprocessed connection request from listen_sock and return client_sock 
    int client_sock = accept(listen_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
    if (client_sock < 0)
    {
        perror("accept");
        return;
    }

    // Find a empty peer slot
    int slot = find_empty_slot();
    if (slot < 0)
    {
        // No empty slot for new peer
        close(client_sock);
        return;
    }

    // Establish a connection
    peers[slot].socket_descriptor = client_sock;
    peers[slot].joined = false; // Peer ID is NOT decided yet, after JOIN, it become true
    peers[slot].files.clear();
    peers[slot].id = 0;
    peers[slot].listen_port = 0;

    // Get peer address using getpeername
    socklen_t len = sizeof(peers[slot].address);
    // Parameters: getpeername(sockfd, buffer to save peer address, size of buffer)
    if (getpeername(client_sock, reinterpret_cast<struct sockaddr*>(&peers[slot].address), &len) < 0)
    {
        perror("getpeername");
        close(client_sock);
        peers[slot].reset();
        return;
    }
}

/*
* Handle JOIN message: Action = 0
* Update registry
* ACTION_JOIN 1 byte and Peer ID 4 bytes: total 5 bytes
*/
void handle_join(int peer_index, uint8_t* buffer, ssize_t len)
{
    if (len < 5) 
    {
        return; // Invalid message
    }

    // Extract peer ID (network byte order)
    uint32_t peer_id_net;
    std::memcpy(&peer_id_net, buffer+1, sizeof(peer_id_net)); //buffer+1 is to skip copying Action_byte on buffer[0]
    uint32_t peer_id = ntohl(peer_id_net);

    peers[peer_index].id = peer_id;
    peers[peer_index].joined = true;
    // The address is already set from getpeername in handle_new_connection()

    // Check if listen port is included
    if (len >= 7)
    {
        uint16_t listen_port_net;
        std::memcpy(&listen_port_net, buffer + 5, sizeof(listen_port_net));
        peers[peer_index].listen_port = ntohs(listen_port_net);
        
        // Update the port in address to use listen_port instead of ephemeral port
        peers[peer_index].address.sin_port = htons(peers[peer_index].listen_port);
        
        std::cout << "TEST] JOIN " << peer_id << " (listen port: " << peers[peer_index].listen_port << ")" << std::endl;
    }
    else
    {
        // use ephemeral port (won't work for FETCH)
        std::cout << "TEST] JOIN " << peer_id << std::endl;
    }
}

/*
* Handle PUBLISH message: Action = 1
* Update files on registry
* [ACTION_PUBLISH 1 byte] [File count 4 bytes] [Filenames\0];
*/
void handle_publish(int peer_index, uint8_t* buffer, ssize_t len)
{
    if (len < 5) 
    {
        return; // Invalid message
    }

    // Extract file count
    uint32_t count_net;
    std::memcpy(&count_net, buffer+1, sizeof(count_net));
    uint32_t count = ntohl(count_net);

    // Clear existing files
    peers[peer_index].files.clear();

    // Parse filenames
    size_t offset = 5; // Skip first 5 bytes (1 action_byte and 4 count_bytes)
    std::vector<std::string> filenames;

    // i count how many files and static_cast<size_t>(len) avoid warning by treating the same type (size_t) as offset
    for (uint32_t i = 0; i < count && offset < static_cast<size_t>(len); i++)
    {
        std::string filename;
        while (offset < static_cast<size_t>(len) && buffer[offset] != '\0')
        {
            filename += static_cast<char>(buffer[offset]);
            offset++;
        }

        // Skip NULL terminator
        offset++;

        if (!filename.empty())
        {
            peers[peer_index].files.push_back(filename);
            filenames.push_back(filename);
        }
    }

    // Print summary: TEST] PUBLISH <count> <filename\0>...
    std::cout << "TEST] PUBLISH " << filenames.size();
    for (const auto& f : filenames)
    {
        std::cout << " " << f;
    }
    std::cout << std::endl;
}

/*
* Handle SEARCH message: Action = 2
* Search a file on registry
* [ACTION_SEARCH 1 byte] [Filename\0];
*/
void handle_search(int peer_index, uint8_t* buffer, ssize_t len)
{
    if (len < 2)
    {
        return; // Invalid message
    }

    // Extract a filnname
    std::string filename;
    size_t offset = 1; // Skip buffer[0] as Action byte
    // static_cast<size_t>(len) avoid warning by treating the same type (size_t) as offset
    while (offset < static_cast<size_t>(len) && buffer[offset] != '\0')
    {
        // static_cast<char>(buffer[offset]) avoid warning by treating the same type (char) as filename
        filename += static_cast<char>(buffer[offset]);
        offset++;
    }

    // Search for the file
    int found_peer = find_file(filename);

    // Prepare response (10 bytes)
    uint8_t response[10];
    uint32_t resp_peer_id = 0;
    uint32_t resp_ip = 0;
    uint16_t resp_port = 0;

    char ip_str[INET_ADDRSTRLEN]; // Use printing ip addr.

    if (found_peer >= 0)
    {
        resp_peer_id = htonl(peers[found_peer].id); 
        resp_ip = peers[found_peer].address.sin_addr.s_addr; // resp_ip is already network byte order
        resp_port = peers[found_peer].address.sin_port; // resp_port is already network byte order

        // Use listen_port if available, otherwise use address port
        if (peers[found_peer].listen_port > 0)
        {
            resp_port = htons(peers[found_peer].listen_port);
        }
        else
        {
            resp_port = peers[found_peer].address.sin_port;
        }
        
        // inet_ntop(address family (IPv4 etc.), Binary IP, buffer, buffer_size)
        // Put readable IP addr. in ip_str
        inet_ntop(AF_INET, &peers[found_peer].address.sin_addr, ip_str, INET_ADDRSTRLEN);
    }
    else
    {
        ip_str[0] = '0';
        ip_str[1] = '.';
        ip_str[2] = '0';
        ip_str[3] = '.';
        ip_str[4] = '0';
        ip_str[5] = '.';
        ip_str[6] = '0';
        ip_str[7] = '\0';
    }

    // Build response having 10 bytes
    std::memcpy(response, &resp_peer_id, 4); // resp_peer_id 4 bytes
    std::memcpy(response+4, &resp_ip, 4); // respo_ip 4 bytes
    std::memcpy(response+8, &resp_port, 2); // resp_port 2 bytes

    // Send response in single send call
    ssize_t sent = send(peers[peer_index].socket_descriptor, response, sizeof(response), 0);
    if (sent != sizeof(response))
    {
        perror("send");
        return;
    }

    // Print summary: TEST] SEARCH <peer_id> <filename> <ip>:port>
    uint32_t peer_id_host = ntohl(resp_peer_id);
    uint16_t port_host = ntohs(resp_port);
    // ntohl(resp_ip) doesn't work and print big integers, so inet_ntop() makes ip addr.,like 1:1:1:1

    std::cout << "TEST] SEARCH " << filename << " " << peer_id_host << " " << ip_str << ":" << port_host << std::endl;
}

/*
* Handle peer message
*/
void handle_peer_message(int peer_index)
{
    uint8_t buffer[BUFFER_SIZE];

    ssize_t len = recv(peers[peer_index].socket_descriptor, buffer, sizeof(buffer), 0);

    if (len <= 0)
    {
        // Connection closed
        remove_peer(peer_index);
        return;
    }

    uint8_t action = buffer[0];

    switch (action)
    {
        case ACTION_JOIN:
            handle_join(peer_index, buffer, len);
            break;
        case ACTION_PUBLISH:
            handle_publish(peer_index, buffer, len);
            break;
        case ACTION_SEARCH:
            handle_search(peer_index, buffer, len);
            break;
        default:
            // Unkown action 
            break;
    }
}

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return 1;
    }

    uint16_t port = static_cast<uint16_t>(std::atoi(argv[1]));
    int listen_sock = setup_listen_socket(port);
    if (listen_sock < 0)
    {
        return 1;
    }

    // Initialize peer table
    for (int i = 0; i < MAX_PEERS; i++)
    {
        peers[i].reset();
    }

    // Set up pollfd array
    std::vector<struct pollfd> fds;

    while (true)
    {
        // Build pollfd array
        fds.clear();

        // Add listen socket
        struct pollfd listen_pfd;
        listen_pfd.fd = listen_sock; // Manage listen_sock
        listen_pfd.events = POLLIN; // Make an action if it gets I/O requests, like making connections
        listen_pfd.revents = 0; // flag
        fds.push_back(listen_pfd);

        // Add connected peer sockets
        for (int i = 0; i < MAX_PEERS; i++) // fds[0] is listen_socket, fds[1~] are peer sockets
        {
            if (peers[i].socket_descriptor >= 0) // Manage only activate peers
            {
                struct pollfd peer_pfd;
                peer_pfd.fd = peers[i].socket_descriptor;
                peer_pfd.events = POLLIN;
                peer_pfd.revents = 0;
                fds.push_back(peer_pfd);
            }
        }

        // Wait for action (no timeout: -1)
        int ret = poll(fds.data(), fds.size(), -1);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("poll");
            break;
        }

        // Check listen socket for new connections
        if (fds[0].revents & POLLIN)
        {
            handle_new_connection(listen_sock);
        }

        // Check peer sockets for messages
        for (size_t i = 1; i < fds.size(); i++) // fd[0] is linten_scoket
        {
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR))
            {
                int peer_index = find_peer_by_socket(fds[i].fd);
                if (peer_index >= 0)
                {
                    handle_peer_message(peer_index);
                }
            }
        }
    }

    // Clean up
    close(listen_sock);
    for (int i = 0; i < MAX_PEERS; i++)
    {
        if (peers[i].socket_descriptor >= 0)
        {
            close(peers[i].socket_descriptor);
        }
    }

    return 0;
}