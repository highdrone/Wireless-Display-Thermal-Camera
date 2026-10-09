// I2C functions for the Melexis MLX90640 driver, on the Arduino Wire library.
#include "mlx90640.h"

extern "C" {
#include "MLX90640_I2C_Driver.h"
}

static TwoWire *bus = &Wire;

void MLX90640_SetWire(TwoWire *wire) { bus = wire; }

extern "C" void MLX90640_I2CInit(void) {}

extern "C" int MLX90640_I2CGeneralReset(void) {
  bus->beginTransmission(0x00);
  bus->write(0x06);
  return bus->endTransmission() == 0 ? 0 : -MLX90640_I2C_NACK_ERROR;
}

// Reads 16-bit big-endian words, in chunks that fit Wire's 128-byte buffer.
extern "C" int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddress, uint16_t nMemAddressRead,
                                uint16_t *data) {
  while (nMemAddressRead > 0) {
    const uint16_t words = nMemAddressRead > 64 ? 64 : nMemAddressRead;
    bus->beginTransmission(slaveAddr);
    bus->write(startAddress >> 8);
    bus->write(startAddress & 0xFF);
    if (bus->endTransmission(false) != 0) return -MLX90640_I2C_NACK_ERROR;
    if (bus->requestFrom((uint16_t)slaveAddr, (size_t)(words * 2), true) != words * 2) return -MLX90640_I2C_NACK_ERROR;
    for (uint16_t i = 0; i < words; i++) {
      const uint8_t hi = bus->read();
      *data++ = (hi << 8) | (uint8_t)bus->read();
    }
    startAddress += words;
    nMemAddressRead -= words;
  }
  return 0;
}

// Like Melexis' reference driver, reads the value back to confirm the write.
extern "C" int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data) {
  bus->beginTransmission(slaveAddr);
  bus->write(writeAddress >> 8);
  bus->write(writeAddress & 0xFF);
  bus->write(data >> 8);
  bus->write(data & 0xFF);
  if (bus->endTransmission() != 0) return -MLX90640_I2C_NACK_ERROR;
  uint16_t check;
  if (MLX90640_I2CRead(slaveAddr, writeAddress, 1, &check) != 0) return -MLX90640_I2C_NACK_ERROR;
  return check == data ? 0 : -MLX90640_I2C_WRITE_ERROR;
}

extern "C" void MLX90640_I2CFreqSet(int freq) { bus->setClock(freq * 1000); }  // freq in kHz
