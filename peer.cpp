#include <iostream>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <filesystem>
#include <vector>
#include <fstream>
#include <poll.h>
#include <errno.h>

// Protocol Action Constants
const uint8_t ACTION_JOIN = 0;
const uint8_t ACTION_PUBLISH = 1;
const uint8_t ACTION_SEARCH = 2;
const uint8_t ACTION_FETCH = 3;

const int BUFFER_SIZE = 4096;

// Global variables
int registry_socket = -1;
int listen_socket = -1;
std::string registry_host;
uint16_t registry_port;
uint16_t listen_port; // Port for accepting FETCH requests
uint32_t peer_id;

// Connected peers: those who connected to us for FETCH
std::vector<int> connected_peers;

/*
* Set up listening socket for incoming FETCH requests
* Return: socket fd on success, -1 on failure
*/ 
int setup_listen_socket(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("socket");
        return -1;
    }

    // Allow address reuse
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt");
        close(sock);
        return -1;
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        perror("bind");
        close(sock);
        return -1;
    }

    if (listen(sock, 5) < 0)
    {
        perror("listen");
        close(sock);
        return -1;
    }

    std::cout << "Listening for FETCH requests on port " << port << std::endl;
    return sock;
}


/*
* Connect the registry server
* Return: socket file descriptor on success or -1 on failure
*/ 
int connect_to_registry()
{
    // Create a TCP socket for IPv4 communication
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) // Verify if socket creation failed
    {
        std::cerr << "Error creating socket" << std::endl;
        return -1;
    }

    // Prepare structure for hostname resolution
    struct addrinfo hints, *result;
    std::memset(&hints, 0, sizeof(hints)); // Initialize hints structure to zero
    hints.ai_family = AF_INET; // Specify IPv4 address family
    hints.ai_socktype = SOCK_STREAM; // Specify TCP socket type


    // Resolve hostname/IP address to network address structure
    std::string port_str = std::to_string(registry_port); // Convert port number from integer to string for getaddrinfo()
    int status = getaddrinfo(registry_host.c_str(), port_str.c_str(), &hints, &result); // c.str makes const char*
    if (status != 0) // Verify if hostname resolution failed
    {
        std::cerr << "Error resolving hostname" << std::endl;
        close(sock);
        return -1;
    }

    // Establish TCP connection to the registry server
    if (connect(sock, result->ai_addr, result->ai_addrlen) < 0)
    {
        std::cerr << "Error connecting to registry" << std::endl;
        freeaddrinfo(result);
        close(sock);
        return -1;
    }

    freeaddrinfo(result);
    return sock;
}

/*
* Handle JOIN command
* Sends JOIN request to registry
*/
void handle_join()
{
    // Establish TCP connection to the registry server
    registry_socket = connect_to_registry(); // Verify if connection failed
    if (registry_socket < 0)
    {
        return;
    }

    // Build JOIN message: [Action = 0][Peer ID (4 Bytes)][Listen Port (2 Bytes)]
    uint8_t buffer[7];
    buffer[0] = ACTION_JOIN; // ACTION_JOIN 1 Byte uint8_t
    uint32_t peer_id_network = htonl(peer_id); // Convert peer ID from host byte order to network byte order
    std::memcpy(buffer+1, &peer_id_network, sizeof(peer_id_network)); // Copy the 4-byte peer ID 
    uint16_t listen_port_network = htons(listen_port); // Convert listen port from host byte order to network byte order
    std::memcpy(buffer + 5, &listen_port_network, sizeof(listen_port_network)); // Copy the 2-byte peer ID

    // Send JOIN request in single send call
    ssize_t sent = send(registry_socket, buffer, sizeof(buffer), 0);
    if (sent != sizeof(buffer)) // Verify if all 7 bytes were sent successfully
    {
        std::cerr << "Error sending JOIN request" << std::endl;
        close(registry_socket);
        registry_socket = -1; // Make socket invalid
    }
    else
    {
        std::cout << "Joined registry with ID " << peer_id << ", listening on port " << listen_port << std::endl;
    }
}

/*
* Handle PUBLISH command
* Publishes files from SharedFiles directory to registry
*/
void handle_publish()
{
    if (registry_socket < 0) // Verify if connected to registry
    {
        std::cerr << "Error not connected to registry. Use JOIN first" << std::endl;
        return;
    }

    // Read files from SharedFiles directory
    std::vector<std::string> files;
    std::string shared_dir = "SharedFiles";

    try
    {
        if (!std::filesystem::exists(shared_dir)) // Verify if SharedFiles directory doesn't exist
        {
            std::cerr << "Error: SharedFiles directory doesn't exist" << std::endl;
            return;
        }

        // Iterate through all entries in SharedFiles directory
        for (const auto& entry : std::filesystem::directory_iterator(shared_dir))
        {
            if (entry.is_regular_file()) // Verify if entry is a regular file (not a directory)
            {
                // Extract filename and add to files vector
                files.push_back(entry.path().filename().string());
            }
        }
    }
    catch(const std::exception& e)
    {
        std::cerr << "Error reading SharedFiles directory" << std::endl;
        return;
    }

    // Build PUBLISH message: [Action = 1][Count (4 Bytes)][File names]
    std::vector<uint8_t> buffer; // Dynamic byte array
    buffer.push_back(ACTION_PUBLISH); // Add ACTION_PUBLISH 1 Byte uint8_t
    
    // Add file count
    /*
    * reinterpret_cast<const uint8_t*>(&count) is to treat count as four 1-byte units (uint_8) 
    */
    uint32_t count = htonl(files.size()); // Convert file count to network byte order
    const uint8_t* count_ptr = reinterpret_cast<const uint8_t*>(&count); // Get pointer to count bytes
    buffer.insert(buffer.end(), count_ptr, count_ptr+sizeof(count)); // Append 4 bytes of count

    // Add file names (NULL-terminated)
    for (const auto& filename : files)
    {
        // Append filename characters to buffer
        buffer.insert(buffer.end(), filename.begin(), filename.end());
        buffer.push_back('\0'); // Add NULL after each filename
    }

    // Send PUBLISH request in single send call
    /*
    * static_cast<ssize_t>(buffer.size()) adjusts type from size_t to ssize_t
    */
    ssize_t sent = send(registry_socket, buffer.data(), buffer.size(), 0);
    if (sent != static_cast<ssize_t>(buffer.size())) // Verify if all bytes were sent successfully
    {
        std::cerr << "Error sending PUBLISH request" << std::endl; 
    }
    else
    {
        std::cout << "Published " << files.size() << " files" << std::endl;
    }
}

/*
* Handle SEARCH command
* Searches for a file in the registry
*/
void handle_search(const std::string& filename_arg = "")
{
    if (registry_socket < 0) // Verify if connected to registry
    {
        std::cerr << "Error not connected to registry. Use JOIN first" << std::endl;
        return;
    }

    // Read file name from user
    std::string filename = filename_arg;
    if (filename.empty())
    {
        std::cout << "Enter a file name: ";
        std::getline(std::cin, filename);
    }
    
    // Build SEARCH message: [Action = 2][File name]
    std::vector<uint8_t> buffer; // Dynamic byte array
    buffer.push_back(ACTION_SEARCH); // Add Action_SEARCH 1 byte uint8_t
    buffer.insert(buffer.end(), filename.begin(), filename.end()); // Append filename characters to buffer
    buffer.push_back('\0'); // Add NULL

    // Send SEARCH request in single send call
    ssize_t sent = send(registry_socket, buffer.data(), buffer.size(), 0);
    if (sent != static_cast<ssize_t>(buffer.size())) // Verify if all bytes were sent successfully
    {
        std::cerr << "Error sending SEARCH request" << std::endl; 
        return;
    }

    // Receive SEARCH response: [Peer ID (4 bytes)][IPv4][Port (2 bytes)]
    uint8_t response[10]; // 4B+4B+2B
    ssize_t received = recv(registry_socket, response, sizeof(response), 0);
    if (received != sizeof(response)) // Verify if All 10 bytes were received
    {
        std::cerr << "Error receiving SEARCH response" << std::endl;
        return; 
    }

    // Extract peer information from byte array
    uint32_t found_peer_id;
    uint32_t found_ip;
    uint16_t found_port;

    std::memcpy(&found_peer_id, response, sizeof(found_peer_id)); // Copy 4 bytes from response to found_peer_id
    std::memcpy(&found_ip, response+4, sizeof(found_ip)); // Copy 4 bytes from response to found_ip
    std::memcpy(&found_port, response+8, sizeof(found_port)); // Copy 2 bytes from response to found_port

    found_peer_id = ntohl(found_peer_id); // Convert peer ID from network byte order to host byte order
    found_port = ntohs(found_port); // Convert port number from network byte order to host byte order

    // Verify if file was found (all zero indicates not found)
    if (found_peer_id == 0 && found_ip == 0 && found_port == 0)
    {
        std::cout << "File not indexed by registry" << std::endl;
    }
    else
    {
        // Convert IP address to string using inet_ntop
        /*
        * INET_AARSTRLEN is 16 bytes of buffer
        * inet_ntop(AF_INET, &found_ip, ip_str, INET_ADDRSTRLEN)
            inet_ntop convert &found_ip from Binary to Human-readable string 
            AF_INET = IPv4, source = &found_ip, destination = ip_str, buffer_size = INET_ADDRSTRLEN
        */
        char ip_str[INET_ADDRSTRLEN]; // buffer with 16 bytes
        inet_ntop(AF_INET, &found_ip, ip_str, INET_ADDRSTRLEN);

        std::cout << "File found at" << std::endl;
        std::cout << "Peer " << found_peer_id << std::endl;
        std::cout << ip_str << ":" << found_port << std::endl;
    }
}

/*
* Handle FETCH command
* Fetches a file from another peer
*/
void handle_fetch()
{
    if (registry_socket < 0) // Verify if connected to registry
    {
        std::cerr << "Error not connected to registry. Use JOIN first" << std::endl;
        return;
    }

    // Read file name from user
    std::string filename;
    std::cout << "Enter a file name: ";
    std::getline(std::cin, filename);


    // Step 1 - SEARCH for the file

    // Build SEARCH message: [Action = 2][File name]
    std::vector<uint8_t> search_buffer; // Dynamic byte array
    search_buffer.push_back(ACTION_SEARCH); // Add Action_SEARCH 1 byte uint8_t
    search_buffer.insert(search_buffer.end(), filename.begin(), filename.end()); // Append filename characters to buffer
    search_buffer.push_back('\0'); // Add NULL

    // Send SEARCH request in single send call
    ssize_t sent = send(registry_socket, search_buffer.data(), search_buffer.size(), 0);
    if (sent != static_cast<ssize_t>(search_buffer.size())) // Verify if all bytes were sent successfully
    {
        std::cerr << "Error sending SEARCH request" << std::endl; 
        return;
    }

    // Receive SEARCH response: [Peer ID (4 bytes)][IPv4][Port (2 bytes)]
    uint8_t search_response[10]; // 4B+4B+2B
    ssize_t received = recv(registry_socket, search_response, sizeof(search_response), 0);
    if (received != sizeof(search_response)) // Verify if All 10 bytes were received
    {
        std::cerr << "Error receiving SEARCH response" << std::endl;
        return; 
    }

    // Extract peer information from byte array
    uint32_t found_peer_id;
    uint32_t found_ip;
    uint16_t found_port;

    std::memcpy(&found_peer_id, search_response, sizeof(found_peer_id)); // Copy 4 bytes from search_response to found_peer_id
    std::memcpy(&found_ip, search_response+4, sizeof(found_ip)); // Copy 4 bytes from search_response to found_ip
    std::memcpy(&found_port, search_response+8, sizeof(found_port)); // Copy 2 bytes from search_response to found_port

    found_peer_id = ntohl(found_peer_id); // Convert peer ID from network byte order to host byte order
    // Keep found_port and found_ip in network byte order

    // Handling cases where the process itself holds the file (avoid deadlocks)
    if (found_peer_id == peer_id)
    {
        std::cout << "You already have this file in SharedFiles/" << filename << std::endl;
        return;
    }

    // Verify if file was found (all zero indicates not found)
    if (found_peer_id == 0 && found_ip == 0 && found_port == 0)
    {
        std::cout << "File not indexed by registry" << std::endl;
        return;
    }


    // Step 2 - Connect to the peer that has the file

    int peer_socket = socket(AF_INET, SOCK_STREAM, 0); // Create a new socket for peer-to-peer connection
    if (peer_socket < 0) // Verify if socket creation failed 
    {
        std::cerr << "Error creating socket for peer connection" << std::endl;
        return;
    }

    // Prepare peer address structure for connection
    struct sockaddr_in peer_addr;
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_addr.s_addr = found_ip;
    peer_addr.sin_port = found_port;

    // Establish TCP connection to the peer
    if (connect(peer_socket, reinterpret_cast<struct sockaddr*>(&peer_addr), sizeof(peer_addr)) < 0)
    {
        std::cerr << "Error connecting to peer" << std::endl;
        close(peer_socket);
        return;
    }


    // Step 3 - Send FETCH request to peer

    // Send FETCH message: [Action = 3][File name]
    std::vector<uint8_t> fetch_buffer; // Dynamic byte array for FETCH request
    fetch_buffer.push_back(ACTION_FETCH); // Add ACTION_FETCH 1 byte uint8_t
    fetch_buffer.insert(fetch_buffer.end(), filename.begin(), filename.end()); // Append filename characters to fetch_buffer
    fetch_buffer.push_back('\0'); // Add NULL

    // Send FETCH request to peer 
    sent = send(peer_socket, fetch_buffer.data(), fetch_buffer.size(), 0);
    if (sent != static_cast<ssize_t>(fetch_buffer.size())) // Verify if all bytes were sent successfully
    {
        std::cerr << "Error sending FETCH request" << std::endl;
        close(peer_socket);
        return;
    }


    // Step 4 - Receive FETCH response from peer

    // Receive FETCH response: [Response (1 Byte)][File data]
    uint8_t response_code; // Variable to store response code (1 byte)
    received = recv(peer_socket, &response_code, sizeof(response_code), 0);
    if (received != sizeof(response_code))
    {
        std::cerr << "Error receiving FETCH response" << std::endl;
        close(peer_socket);
        return;
    }

    // Verify if response code was received
    if (response_code != 0)
    {
        std::cerr << "Peer unable to send file" << std::endl;
        close(peer_socket);
        return;
    }


    // Step 5 - Receive and save file data

    // Check that the SharedFiles directory exists (create it if it doesn't)
    std::filesystem::create_directories("SharedFiles");
    
    // Save to SharedFiles
    std::string output_path = "SharedFiles/" + filename;
    std::ofstream outfile(output_path, std::ios::binary); // Create output file stream in binary mode

    
    if (!outfile) // Verify if file creation failed
    {
        std::cerr << "Error creating output file" << std::endl;
        close(peer_socket);
        return;
    }

    // Read file data in chunks
    uint8_t file_buffer[4096];
    while (true) // Loop until all file data is received
    {
        // Receive up to 4096 bytes of file data
        ssize_t bytes_received = recv(peer_socket, file_buffer, sizeof(file_buffer), 0);
        if (bytes_received < 0)
        {
            std::cerr << "Error receiving file data" << std::endl;
            outfile.close();
            close(peer_socket);
            return;
        }
        if (bytes_received == 0) // No more data to receive
        {
            // Connection closed by peer
            break;
        }
        
        // Write received data to output file
        outfile.write(reinterpret_cast<char*>(file_buffer), bytes_received);
    }

    outfile.close();
    close(peer_socket);
    std::cout << "File downloaded successfully: " << output_path << std::endl;
}

/*
* Handle incoming FETCH request (server side - send file to another peer)
* Format: [Action=3][Filename\0]
* Response: [success=0/fail=1][file data]
*/
void handle_incoming_fetch(int client_sock)
{
    uint8_t buffer[BUFFER_SIZE];
    ssize_t len = recv(client_sock, buffer, sizeof(buffer), 0);
    
    if (len <= 0)
    {
        close(client_sock);
        return;
    }

    // Check action code
    if (buffer[0] != ACTION_FETCH)
    {
        uint8_t error_response = 1;
        send(client_sock, &error_response, 1, 0);
        close(client_sock);
        return;
    }

    // Extract filename
    std::string filename;
    size_t offset = 1; // Skip the first byte(ACTION_FETCH)
    while (offset < static_cast<size_t>(len) && buffer[offset] != '\0')
    {
        filename += static_cast<char>(buffer[offset]);
        offset++;
    }

    std::cout << "Received FETCH request for: " << filename << std::endl;

    // Try to open the file
    std::string filepath = "SharedFiles/" + filename;
    std::ifstream infile(filepath, std::ios::binary);
    
    if (!infile)
    {
        std::cerr << "File not found: " << filepath << std::endl;
        uint8_t error_response = 1;
        send(client_sock, &error_response, 1, 0);
        close(client_sock);
        return;
    }

    // Send success response
    uint8_t success_response = 0;
    if (send(client_sock, &success_response, 1, 0) != 1)
    {
        std::cerr << "Error sending response code" << std::endl;
        infile.close();
        close(client_sock);
        return;
    }

    // Send file data
    char file_buffer[4096];
    while (infile.read(file_buffer, sizeof(file_buffer)) || infile.gcount() > 0)
    {
        ssize_t to_send = infile.gcount();
        ssize_t sent = send(client_sock, file_buffer, to_send, 0);
        if (sent != to_send)
        {
            std::cerr << "Error sending file data" << std::endl;
            break;
        }
    }

    infile.close();
    close(client_sock);
    std::cout << "File sent successfully: " << filename << std::endl;
}

/*
* Handle new peer connection (for incoming FETCH)
*/
void handle_new_peer_connection()
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_sock = accept(listen_socket, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
    if (client_sock < 0)
    {
        perror("accept");
        return;
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    std::cout << "Incoming connection from " << ip_str << ":" << ntohs(client_addr.sin_port) << std::endl;

    // Handle the FETCH request immediately
    handle_incoming_fetch(client_sock);
}

/*
* Process user command
* Returns false if EXIT command
*/
bool process_command(const std::string& command)
{
    if (command == "JOIN")
    {
        handle_join();
    }
    else if (command == "PUBLISH")
    {
        handle_publish();
    }
    else if (command == "SEARCH")
    {
        handle_search();
    }
    else if (command == "FETCH")
    {
        handle_fetch();
    }
    else if (command == "EXIT")
    {
        if (registry_socket >= 0) 
        {
            close(registry_socket);
            registry_socket = -1;
        }
        return false;
    }
    else if (!command.empty())
    {
        std::cerr << "Unknown command: " << command << std::endl;
    }
    return true;
}

int main(int argc, char* argv[])
{
    // Check command line arguments
    if (argc != 5)
    {
        std::cerr << "Usage: " << argv[0] << " <registry_host> <registry_port> <peer_id> <listen_port>" << std::endl;
        std::cerr << "  registry_host: Hostname/IP of the registry server" << std::endl;
        std::cerr << "  registry_port: Port number of the registry server" << std::endl;
        std::cerr << "  peer_id:       Unique ID for this peer" << std::endl;
        std::cerr << "  listen_port:   Port to listen for incoming FETCH requests" << std::endl;
        return 1;
    }

    // Parse command line arguments
    registry_host = argv[1];
    registry_port = static_cast<uint16_t>(std::stoi(argv[2]));
    peer_id = static_cast<uint32_t>(std::stoul(argv[3]));
    listen_port = static_cast<uint16_t>(std::stoi(argv[4]));

    // Set up listen socket for incoming FETCH requests
    listen_socket = setup_listen_socket(listen_port);
    if (listen_socket < 0)
    {
        return 1;
    }

    std::cout << "P2P Peer started. ID: " << peer_id << std::endl;
    std::cout << "Commands: JOIN, PUBLISH, SEARCH, FETCH, EXIT" << std::endl;

    // Main event loop using poll
    std::vector<struct pollfd> fds;
    bool running = true;

    while (running)
    {
        fds.clear();

        // Add stdin (fd 0) for user input
        struct pollfd stdin_pfd;
        stdin_pfd.fd = STDIN_FILENO; // 0
        stdin_pfd.events = POLLIN;
        stdin_pfd.revents = 0;
        fds.push_back(stdin_pfd);

        // Add listen socket for incoming FETCH connections
        struct pollfd listen_pfd;
        listen_pfd.fd = listen_socket;
        listen_pfd.events = POLLIN;
        listen_pfd.revents = 0;
        fds.push_back(listen_pfd);

        std::cout << "Enter a command: " << std::flush;

        // Wait for activity
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

        // Check stdin for user input
        if (fds[0].revents & POLLIN)
        {
            std::string command;
            std::getline(std::cin, command);
            running = process_command(command);
        }

        // Check listen socket for incoming connections
        if (fds[1].revents & POLLIN)
        {
            handle_new_peer_connection();
        }
    }

    // Cleanup
    if (listen_socket >= 0)
    {
        close(listen_socket);
    }
    if (registry_socket >= 0)
    {
        close(registry_socket);
    }

    std::cout << "Peer exited." << std::endl;
    return 0;
}
