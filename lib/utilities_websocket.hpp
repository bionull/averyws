//          Matthew Avery Coder 2012 - 2013.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
#ifndef UTILITIES_WEBSOCKET
#define UTILITIES_WEBSOCKET

#include <cstdint>

namespace Utilities {

class Websocket {
  public:
    static uint64_t reserve(const uint64_t length , const uint16_t flags);
    static void makeHeader(uint8_t *data, const uint64_t data_size, const uint64_t length,const uint16_t flags);
    static void generateMask(uint8_t *data);
    static void flags2header(uint8_t *data, uint16_t flags);
    static uint8_t length2header(uint8_t *data, uint64_t length);
    static void applyMask(uint8_t* it,const uint64_t length,uint8_t* mask,uint8_t mask_offset);
  private:
    Websocket();
    Websocket(const Websocket&);
    Websocket& operator=(const Websocket&);

  
};
}

#endif
