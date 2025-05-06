# Multithreaded HTTP Server with Atomic Requests and Coherent Logs

`httpserver` is a multithreaded http server that runs on localhost and serves atomic file requests from multiple clients. The server distributes work amongst threads by using a work queue and it coherently logs the order of requests. It implements two standard http methods (`PUT` and `GET`)

## Design

### 1. HTTP Methods
1. `GET`
   * `httpserver` recieves an object name and sends back the contents of that object to the client
2. `PUT`
   * `httpserver` recieves an object name and data content. It writes the data into the specified object if it exists, otherwise it creates it 

### 2. Client Response Codes
* `200 OK`
  * this response is sent to clients over the connection when a `GET`, `PUT`, or `APPEND` request is completed successfully
* `201 CREATED`
  * this response is sent to clients over the connection when a `PUT` request is successful and it created the requested file
* `400 BAD REQUEST`
  * this response is sent to clients over the connection when their request is ill formatted or missing necessary header fields
* `403 FORBIDDEN`
  * this response is sent to clients over the connection when the file they requested is not accessible (access permissions)
* `404 FILE NOT FOUND`
  * this response is sent to clients over the connection when the file they requested on a `GET` or `APPEND` does not exist
* `500 INTERNAL SERVER ERROR`
  * this response is sent to clients over the connection when an error is encountered within the functionality of the server
* `501 NOT IMPLEMENTED`
  * this response is sent to clients over the connection when the method they requested is not implemented or does not exist
 

  ## Building

    $ make
    $ make all

## Running

    $ ./httpserver <portnumber> -t <threads> -l <logfile>
        * portnumber: binding port for httpserver to listen for requests and serve them
        * -t <threads>: number of threads running in the httpserver
        * -l <logfile>: specifies a logfile for output

## Formatting

    $ make format
    
## Cleaning

    $ make clean

### . Limitations
1. `httpserver` does not work across different networks
2. `httpserver` ignores most header fields (ex: hostname)
3. `httpserver` only supports 2 standard http methods (`PUT` and `GET`)
4. `httpserver` is not completely "coherent" or "atomic" in logging

## NOTE : 

    you know who you are if you shouldn't be looking at this peeps. As Bookroo once said "The consequences of every act are included in the act itself."
