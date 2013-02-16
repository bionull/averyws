#include <ws_session.hpp>

#include <boost/asio.hpp>

#include <utilities_websocket.hpp>
#include <utilities_chunk_vector.hpp>
#include <ws_session_manager.hpp>

#include <iostream>
#include <sstream>
#include <iomanip>

#include <system_error>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <memory>

#include <array>
#include <string>


#include <openssl/sha.h>
#include <base64.hpp>

#include <zmq.hpp>


namespace {
//for zmq no copy
using namespace Utilities;
void freeSharedChunk(void* data,void* hint) {
    reinterpret_cast<ChunkVector_sp *>(hint)->reset();

}

}

namespace Websocket {

using namespace boost;
using namespace boost::asio;
using namespace Utilities;

  Session::Session(io_service& io_service
          , SessionManager &session_manager
          ,   zmq::socket_t &socket_pub
          )
  :  socket_(io_service)
   , strand_(io_service)
   , io_service_(io_service)
   , session_manager_(session_manager)
   , socket_pub_(socket_pub)
   , authenticated_(false){
  }

  ip::tcp::socket& Session::socket() {
    return socket_;
  }

  void Session::start() {
    session_manager_.add_unauthed(shared_from_this());
    std::cout << "in session start" << std::endl;
    buffer_ = ChunkVector_sp ( new ChunkVector());
    buffer_->new_chunk();

    new_request_ = true;
    socket_.async_read_some(
              buffer(&buffer_->at(0), buffer_->size())
            , strand_.wrap([&](const system::error_code& error, size_t bytes_transferred) {
                if (!error) {
                  std::string data(reinterpret_cast<char*>(&buffer_->at(0)),bytes_transferred);
                  first_handshake_size_ = bytes_transferred;
                  
                  //add this task to a write queue OR send it to a 'task manager that creates'
                  std::cout << "first handshake\n" << data;
                  static const std::string magic_number = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
                  static const std::string search = "\r\nSec-WebSocket-Key: ";
                  size_t found = data.find(search);

                  if(found == std::string::npos){
                      std::cout << "shit" << std::endl;
                  }

                  size_t found_end = data.find("\r\n",found+search.size());
                  std::string input =  data.substr(found+search.size(),found_end-found-search.size()) + magic_number;
                  unsigned char obuf[20];
                  SHA1(reinterpret_cast<const unsigned char *>(input.c_str()),input.size(),obuf);
                  for (int i = 0; i < 20; i++) {
                      std::cout << std::hex << static_cast<unsigned int>(obuf[i]) << " ";
                  }
                  std::cout << std::endl;

                  std::ostringstream oss;
                  oss << "HTTP/1.1 101 Switching Protocols\r\n"
                      << "Upgrade: websocket\r\n"
                      << "Connection: Upgrade\r\n"
                      << "Sec-WebSocket-Accept: " << base64_encode(obuf,20)
                      << "\r\n\r\n";
                  std::string temp = oss.str(); 

                  std::copy(temp.begin(),temp.end(),&buffer_->at(0));
                  buffer_->at(temp.size()) = '\0';

                  auto this_shared = shared_from_this();
                  async_write(socket_
                        , buffer(&buffer_->at(0), temp.size())
                        , this_shared->strand_.wrap([&this_shared](const system::error_code& error, size_t bytes_transferred) {
                            if (!error) {
                                this_shared->read_header();
                                std::cout << "no error" << std::endl;
                            } else {
                                this_shared->session_manager_.remove(this_shared);
                            }
                        }));

                  std::cout << "response:" << (const char *) &buffer_->at(0) << std::endl;
                } else {
                  session_manager_.remove(shared_from_this());
                }
    }));
  }

  void Session::read_header() {
    std::cout << "in read header" << std::endl;
    temp_header_buffer_ = std::make_shared<std::array<uint8_t,14> >();
    async_read(socket_ ,
            buffer(&temp_header_buffer_->at(0), temp_header_buffer_->size()) ,
            transfer_exactly(2) ,
            strand_.wrap(std::bind(&Session::handle_read_header_1, shared_from_this(), std::placeholders::_1)));
  }

  void Session::handle_read_header_1(const system::error_code& error) {
    std::cout << "in read header 1" << std::endl;
    std::cout << error << std::endl;
    if (!error) {
    temp_payload_size_ = temp_header_buffer_->at(1) & 0x7F;
    temp_mask_ = 0;
    

    uint64_t read = 0;
    if(temp_payload_size_ <= 125) {
        read = 0;
    } else if(temp_payload_size_ == 126) {
        read = 1;
    } else if (temp_payload_size_ == 127) {
        read = 8;
    }

    if(temp_header_buffer_->at(1) & 0x80) {
        temp_mask_ = &temp_header_buffer_->at(2 + read);
        read += 4;
    } else {
        temp_mask_ = &temp_header_buffer_->at(10);
        std::fill(temp_mask_, temp_mask_ + 4 ,0);
    }


    async_read(socket_ ,
            buffer(&temp_header_buffer_->at(2),temp_header_buffer_->size()) , 
            transfer_exactly(read) ,
            strand_.wrap(std::bind(&Session::handle_read_header_2, shared_from_this(), std::placeholders::_1)));
    } else {
      session_manager_.remove(shared_from_this());
    }
  }

  void Session::handle_read_header_2(const system::error_code& error) {
    std::cout << "in read header 2" << std::endl;
    if (!error) {
    if(temp_payload_size_ == 126) {
        temp_payload_size_ = (static_cast<uint64_t>(temp_header_buffer_->at(2)) << 8) 
                           | (static_cast<uint64_t>(temp_header_buffer_->at(3)) << 0);
    } else if (temp_payload_size_ == 127) {
        temp_payload_size_ = (static_cast<uint64_t>(temp_header_buffer_->at(2)) << 56) 
                           | (static_cast<uint64_t>(temp_header_buffer_->at(3)) << 48)
                           | (static_cast<uint64_t>(temp_header_buffer_->at(4)) << 40)
                           | (static_cast<uint64_t>(temp_header_buffer_->at(5)) << 32)
                           | (static_cast<uint64_t>(temp_header_buffer_->at(6)) << 24)
                           | (static_cast<uint64_t>(temp_header_buffer_->at(7)) << 16)
                           | (static_cast<uint64_t>(temp_header_buffer_->at(8)) <<  8) 
                           | (static_cast<uint64_t>(temp_header_buffer_->at(9)) <<  0);
    }
    
    if(   (temp_header_buffer_->at(0) & 0x08) == 0x08
       || (temp_header_buffer_->at(0) & 0x09) == 0x09 
       || (temp_header_buffer_->at(0) & 0x0A) == 0x0A ) {
       std::cout << std::hex << static_cast<unsigned int>(temp_header_buffer_->at(0))
               << " " << static_cast<unsigned int>(temp_header_buffer_->at(1)) << std::endl;
        std::cout << "control header" << std::endl;
        control_buffer_ = std::shared_ptr<std::array<uint8_t,127> >(new std::array<uint8_t,127>);
        if(temp_payload_size_ > 125) {
            std::cout << "bad client" << std::endl;
            session_manager_.remove(shared_from_this());
            return;
        }
        async_read(socket_ ,
                buffer(&control_buffer_->at(0),temp_payload_size_) , 
                transfer_exactly(temp_payload_size_),
                strand_.wrap(std::bind(&Session::handle_control_read, shared_from_this(), std::placeholders::_1)));
        return;
    }
    
    if(temp_payload_size_ == 0) {
        std::cout << "no payload" << std::endl;
        read_header();
        return;
    }

    if(new_request_) {
        payload_read_ = 0;
        payload_size_ = 0;
        buffer_ = ChunkVector_sp( new ChunkVector());
    }

    header_buffer_ =  temp_header_buffer_;
    mask_ = temp_mask_;
    payload_size_ += temp_payload_size_;


    uint64_t read = payload_size_ - payload_read_;
    if(payload_size_ > buffer_->size()) {
        buffer_->new_chunk();
    }
    
    if(read > buffer_->size()) {
        read = buffer_->size() - payload_read_;
    }

    async_read(socket_
        , buffer(&buffer_->at(payload_read_),buffer_->size() -payload_read_)
        , transfer_exactly(read)
        , strand_.wrap(std::bind(&Session::handle_read, shared_from_this(), std::placeholders::_1)));
    } else {
      session_manager_.remove(shared_from_this());
    }
  }


  void Session::handle_read(const system::error_code& error) {
    std::cout << "in handle_read" << std::endl;
    if (!error) {
      if(buffer_->size() >= payload_size_) {
          payload_read_ = payload_size_;
          //everything has been read in this frame.
          if(header_buffer_->at(0) & 0x80) {
              //FIN is set to true so we close the buffer
              buffer_->close_last_chunk(payload_size_ % (buffer_->chunk_size()+1) );
              new_request_ = true;
          } else {
              new_request_ = false;
          }
          Utilities::Websocket::applyMask(&buffer_->at(payload_size_-temp_payload_size_),temp_payload_size_,mask_,0);
          std::cout << std::string(reinterpret_cast<char*>(&buffer_->at(payload_size_-temp_payload_size_)),temp_payload_size_) << std::endl;
          read_header();
      } else {
          payload_read_ = buffer_->size();
          if(payload_read_ % buffer_->chunk_size() == 0) {
             buffer_->new_chunk();
          }
          uint64_t read = payload_size_ - payload_read_;
          if(read > buffer_->size()) {
              read = buffer_->size() - payload_read_;
          }
          async_read(socket_
              , buffer(&buffer_->at(payload_read_),buffer_->size() -payload_read_)
              , transfer_exactly(read)
              , strand_.wrap(std::bind(&Session::handle_read, shared_from_this(), std::placeholders::_1)));
      }
    } else {
      session_manager_.remove(shared_from_this());
    }
  }
  
  void Session::handle_control_read(const system::error_code& error) {
    if (!error) {
        Utilities::Websocket::applyMask(&control_buffer_->at(0),temp_payload_size_,temp_mask_,0);
        std::cout << "in control handler" << std::endl;
        std::cout << std::string(reinterpret_cast<char*>(&control_buffer_->at(0)),temp_payload_size_) << std::endl;
        //TODO do control logic here
        read_header();
    } else {
      session_manager_.remove(shared_from_this());
    }
  }
  
  void Session::request(std::shared_ptr<Utilities::ChunkVector > request) {
      for(auto chunk = request->chunk_cbegin(); chunk != request->chunk_cend(); ++chunk) {
          chunk_sp *shared = new chunk_sp(*chunk);
          zmq::message_t zmq_request ( &(*chunk)->at(0)
                  ,(*chunk)->size()
                  ,freeSharedChunk
                  ,shared);
          if(chunk+1 == request->chunk_cend()) {
              socket_pub_.send(zmq_request);
          } else {
              socket_pub_.send(zmq_request,ZMQ_SNDMORE);
          }

      }
  }

void Session::write(std::shared_ptr<zmq::message_t > message) {
    std::shared_ptr<uint8_t> header(new uint8_t[Utilities::Websocket::reserve(message->size(),0x8080)], std::default_delete<uint8_t[]>());
    auto this_shared = shared_from_this();
    async_write(socket_
          , buffer(message->data(), message->size())
          , strand_.wrap([&this_shared](const system::error_code& error,size_t bytes_transferred){
                if (error) {
                  this_shared->session_manager_.remove(this_shared);
                }
          }));
}

}