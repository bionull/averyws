//          Matthew Avery Coder 2012 - 2013.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
#include <ws_session.hpp>

#include <boost/asio.hpp>

#include <utilities_websocket.hpp>
#include <utilities_chunk_vector.hpp>
#include <ws_session_manager.hpp>

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

#include <ws_dealer.hpp>

#include <cstring>

#include <utilities_print.hpp>


namespace Websocket {

using namespace boost;
using namespace boost::asio;
using namespace Utilities;

  Session::Session(io_service& io_service
          , SessionManager &session_manager
          , Dealer &dealer
          , strand &io_strand
          )
  :  socket_(io_service)
   , strand_(io_service)
   , io_service_(io_service)
   , session_manager_(session_manager)
   , authenticated_(false)
   , dealer_(dealer)
   , io_strand_(io_strand){
  }

  ip::tcp::socket& Session::socket() {
    return socket_;
  }

  void Session::start() {
    session_manager_.add_unauthed(shared_from_this());
    buffer_ = ChunkVector_sp ( new ChunkVector());
    buffer_->new_chunk();

    new_request_ = true;
    socket_.async_read_some(
              buffer(&buffer_->at(0), buffer_->size())
            , strand_.wrap(std::bind(&Session::read_first_header,shared_from_this(),std::placeholders::_1,std::placeholders::_2)));
  }

  void Session::read_first_header(const system::error_code& error, size_t bytes_transferred) {
      if (error) { session_manager_.remove(shared_from_this()); return;}
      static const char magic_number[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
      static const char search[] = "\r\nSec-WebSocket-Key: ";
      static const char response[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Accept: ";
      static const char response_end[] = "\r\n\r\n";

      //null terminate
      buffer_->at(bytes_transferred-1) = '\0';
      first_handshake_size_ = bytes_transferred;

      auto found = std::strstr(reinterpret_cast<char *>(&buffer_->at(0)),search);
      found += (sizeof(search) -1);
      if(found == 0){
          std::cout << "could not find key" << std::endl;
          session_manager_.remove(shared_from_this());
          return;
      }

      auto found_end = std::strstr(found+sizeof(search),"\r\n");
      
      if(found_end == 0){
          std::cout << "could not find end of key" << std::endl;
          session_manager_.remove(shared_from_this());
          return;
      }

      //magic number is null terminated so no need to add an extra character
      uint64_t size = sizeof(magic_number) + (found_end - found);
      std::unique_ptr<uint8_t[]> input(new uint8_t[size]);

      std::copy(found,found_end,input.get());
      //magic number is null terminated so no need to null terminate
      std::copy(magic_number,magic_number + sizeof(magic_number)-1,input.get() + (found_end - found));
      uint8_t obuf[20];
      SHA1(input.get(),size-1,obuf);

      //Utilities::Print::hex(obuf,sizeof(obuf)); std::cout << std::endl;
      //todo this is fixed size, use char[]
      std::string b64 = base64_encode(obuf,20);

      std::copy(response,response + (sizeof(response)-1),&buffer_->at(0));
      std::copy(b64.c_str(),b64.c_str() + b64.size(),&buffer_->at(sizeof(response)-1));
      std::copy(response_end,response_end + (sizeof(response_end)-1),&buffer_->at(sizeof(response)-1+b64.size()));

      auto this_shared = shared_from_this();
      async_write(socket_
              , buffer(&buffer_->at(0), sizeof(response)-1+b64.size()+sizeof(response_end)-1)
              , strand_.wrap([this_shared](const system::error_code& error, size_t bytes_transferred) {
                  if (error) { 
                    std::cout << "error" << std::endl;
                    this_shared->session_manager_.remove(this_shared); 
                    return;
                  }
                  this_shared->read_header();
              }
              ));
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

        std::cout << "control header";
        Utilities::Print::hex(&temp_header_buffer_->at(0),2);
        std::cout << std::endl;

        if(temp_payload_size_ > 125) {
            std::cout << "bad client sending too much payload in control header" << std::endl;
            session_manager_.remove(shared_from_this());
            return;
        }
        control_buffer_ = std::shared_ptr<std::array<uint8_t,127> >(new std::array<uint8_t,127>);
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
      //typical scatther-gather logic
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
          io_service_.post(io_strand_.wrap(std::bind(&Session::request, shared_from_this(),buffer_)));
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
      dealer_.send_open();
      for(auto chunk = request->chunk_begin(); chunk != request->chunk_cend(); ++chunk) {
          if(chunk+1 == request->chunk_end()) {
              dealer_.send_last(std::move(*chunk));
          } else {
              dealer_.send_more(std::move(*chunk));
          }

      }
      dealer_.send_close();
  }

void Session::write(std::shared_ptr<uint8_t> data, uint64_t size) {
    auto deleter = [](uint8_t *ptr){
        std::cout << "deleting header" << std::endl;
        delete[] ptr;
    };
    uint64_t header_size = Utilities::Websocket::reserve(size,0x8100);
    std::shared_ptr<uint8_t> header(new uint8_t[header_size], deleter);


    Utilities::Websocket::makeHeader(header.get(),header_size,header_size,0x8100);

    //double braces until the compiler supports single braces:
    //http://gcc.gnu.org/bugzilla/show_bug.cgi?id=25137
    
    std::array<const_buffer,2> buffers = {{
        buffer(header.get(),header_size),
        buffer(data.get(),size) }};
   
    auto this_shared = shared_from_this();
             
    std::cout << "sending:" << std::string(reinterpret_cast<char *>(header.get()),header_size) << std::endl;
    for(uint64_t i = 0; i < header_size; ++i) std::cout << std::hex << static_cast<unsigned int>(header.get()[i]) << " ";
    std::cout << std::endl;
    std::cout << "sending:" << std::string(reinterpret_cast<char *>(data.get()),size) << " " << size << std::endl;

    std::cout << "header use count: " << header.use_count() << " " << data.use_count() << std::endl;

    async_write(socket_
          , buffers 
          , strand_.wrap([&,data,header](const system::error_code& error,size_t bytes_transferred){
                std::cout << "async header use count: " << header.use_count() << " " << data.use_count() << std::endl;
                if (error) {
                  std::cout << "write error" << std::endl;
                  this_shared->session_manager_.remove(this_shared);
                }
          }));
}

}
