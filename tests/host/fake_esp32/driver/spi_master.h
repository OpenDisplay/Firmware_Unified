#ifndef FAKE_DRIVER_SPI_MASTER_H
#define FAKE_DRIVER_SPI_MASTER_H
#include "fake_esp32.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { SPI1_HOST = 0, SPI2_HOST = 1, SPI3_HOST = 2 } spi_host_device_t;
#define SPI_DMA_CH_AUTO 3
#define SPI_TRANS_USE_RXDATA (1u << 2)
#define SPI_TRANS_USE_TXDATA (1u << 3)
#define SPI_DEVICE_HALFDUPLEX (1u << 4)
typedef struct fake_spi_dev *spi_device_handle_t;
typedef struct {
    uint32_t flags;
    size_t length;
    size_t rxlength;
    void *user;
    union { const void *tx_buffer; uint8_t tx_data[4]; };
    union { void *rx_buffer; uint8_t rx_data[4]; };
} spi_transaction_t;
typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;
typedef struct {
    int mode;
    int clock_speed_hz;
    int spics_io_num;
    uint32_t flags;
    int queue_size;
} spi_device_interface_config_t;
esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *cfg, int dma);
esp_err_t spi_bus_free(spi_host_device_t host);
esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *cfg,
                             spi_device_handle_t *out);
esp_err_t spi_bus_remove_device(spi_device_handle_t dev);
esp_err_t spi_device_polling_transmit(spi_device_handle_t dev, spi_transaction_t *t);
#ifdef __cplusplus
}
#endif
#endif
