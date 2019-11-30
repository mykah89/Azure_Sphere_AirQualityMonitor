#include "pms.h"
#include <stdint.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>

#define UART_STRUCTS_VERSION 1

// applibs_versions.h defines the API struct versions to use for applibs APIs.
#include "applibs_versions.h"
#include "epoll_timerfd_utilities.h"
#include <applibs/uart.h>
#include <applibs/gpio.h>
#include <applibs/log.h>

#include <time.h>

uint16_t makeWord(uint8_t high, uint8_t low) {
	return (high << 8) | (low);
}
//#define MAKEWORD(x, y) ((y) << 8 | (x))

// By default, this sample is targeted at the MT3620 Reference Development Board (RDB).
// This can be changed using the project property "Target Hardware Definition Directory".
// This #include imports the sample_hardware abstraction from that hardware definition.
//#include <hw/sample_hardware.h>

// File descriptors - initialized to invalid value
static int _pms_uartFd = -1;
static int _pms_epollFd = -1;

/// <summary>
///     Helper function to send a fixed message via the given UART.
/// </summary>
/// <param name="uartFd">The open file descriptor of the UART to write to</param>
/// <param name="dataToSend">The data to send over the UART</param>
static void SendUartMessage(int uartFd, const uint8_t *dataToSend, size_t length)
{
	size_t totalBytesSent = 0;
	size_t totalBytesToSend = length;
	int sendIterations = 0;
	while (totalBytesSent < totalBytesToSend) {
		sendIterations++;

		// Send as much of the remaining data as possible
		size_t bytesLeftToSend = totalBytesToSend - totalBytesSent;
		const uint8_t *remainingMessageToSend = dataToSend + totalBytesSent;
		ssize_t bytesSent = write(uartFd, remainingMessageToSend, bytesLeftToSend);
		if (bytesSent < 0) {
			Log_Debug("ERROR: Could not write to UART: %s (%d).\n", strerror(errno), errno);
			//terminationRequired = true;
			return;
		}

		totalBytesSent += (size_t)bytesSent;
	}

	Log_Debug("Sent %zu bytes over UART in %d calls.\n", totalBytesSent, sendIterations);
}

/// <summary>
///     Handle UART event: if there is incoming data, print it.
/// </summary>
static void UartEventHandler(EventData *eventData)
{
	const size_t receiveBufferSize = 256;
	uint8_t receiveBuffer[receiveBufferSize + 1]; // allow extra byte for string termination
	ssize_t bytesRead;

	// Read incoming UART data. It is expected behavior that messages may be received in multiple
	// partial chunks.
	bytesRead = read(_pms_uartFd, receiveBuffer, receiveBufferSize);
	if (bytesRead < 0) {
		Log_Debug("ERROR: Could not read UART: %s (%d).\n", strerror(errno), errno);
		//terminationRequired = true;
		return;
	}

	if (bytesRead > 0) {
		// Null terminate the buffer to make it a valid string, and print it
		receiveBuffer[bytesRead] = 0;
		Log_Debug("UART received %d bytes: '%s'.\n", bytesRead, (char *)receiveBuffer);

		// process sensor data
		for (int i = 0; i < bytesRead; ++i) {
			pms_processData(receiveBuffer[i]);
		}
	}
}

// event handler data structures. Only the event handler field needs to be populated.
static EventData uartEventData = { .eventHandler = &UartEventHandler };

/// <summary>
///     Set up SIGTERM termination handler, initialize peripherals, and set up event handlers.
/// </summary>
/// <returns>0 on success, or -1 on failure</returns>
static int InitPeripheralsAndHandlers(UART_Id uartId, int epollFd)
{
	_pms_epollFd = epollFd;
		//= CreateEpollFd();
	if (_pms_epollFd < 0) {
		Log_Debug("ERROR: Could not create Epoll FD: %s (%d)\n", strerror(errno), errno);
		return -1;
	}

	// Create a UART_Config object, open the UART and set up UART event handler
	UART_Config uartConfig;
	UART_InitConfig(&uartConfig);
	uartConfig.baudRate = 9600;
	uartConfig.flowControl = UART_FlowControl_None;
	_pms_uartFd = UART_Open(uartId, &uartConfig);
	if (_pms_uartFd < 0) {
		Log_Debug("ERROR: Could not open UART: %s (%d).\n", strerror(errno), errno);
		return -1;
	}
	if (RegisterEventHandlerToEpoll(epollFd, _pms_uartFd, &uartEventData, EPOLLIN) != 0) {
		Log_Debug("ERROR: UART handler: %s (%d).\n", strerror(errno), errno);
		return -1;
	}

	return 0;
}

/// <summary>
///     Close peripherals and handlers.
/// </summary>
static void ClosePeripheralsAndHandlers(void)
{
	Log_Debug("Closing file descriptors.\n");
	CloseFdAndPrintError(_pms_uartFd, "Uart");
	CloseFdAndPrintError(_pms_epollFd, "Epoll");
}

void pms_init(UART_Id uartId, int epollFd)
{
	//this->_uartId = uartId;

	if (InitPeripheralsAndHandlers(uartId, epollFd) != 0) {
		Log_Debug("InitPeripherals FAILED.\n");
		//terminationRequired = true;
	}

	Log_Debug("InitPeripherals OK.\n");
	//pms = this;

	_pms_data.PM_SP_UG_1_0 = 0xFFFF;
}

// Standby mode. For low power consumption and prolong the life of the sensor.
void pms_sleep()
{
	uint8_t command[] = { 0x42, 0x4D, 0xE4, 0x00, 0x00, 0x01, 0x73 };
	SendUartMessage(_pms_uartFd, command, sizeof(command));
	//_stream->write(command, sizeof(command));
}

// Operating mode. Stable data should be got at least 30 seconds after the sensor wakeup from the sleep mode because of the fan's performance.
void pms_wakeUp()
{
	uint8_t command[] = { 0x42, 0x4D, 0xE4, 0x00, 0x01, 0x01, 0x74 };
	SendUartMessage(_pms_uartFd, command, sizeof(command));
	//_stream->write(command, sizeof(command));
}

// Active mode. Default mode after power up. In this mode sensor would send serial data to the host automatically.
void pms_activeMode()
{
	uint8_t command[] = { 0x42, 0x4D, 0xE1, 0x00, 0x01, 0x01, 0x71 };
	SendUartMessage(_pms_uartFd, command, sizeof(command));
	//_stream->write(command, sizeof(command));
	_pms_mode = MODE_ACTIVE;
}

// Passive mode. In this mode sensor would send serial data to the host only for request.
void pms_passiveMode()
{
	uint8_t command[] = { 0x42, 0x4D, 0xE1, 0x00, 0x00, 0x01, 0x70 };
	SendUartMessage(_pms_uartFd, command, sizeof(command));
	//_stream->write(command, sizeof(command));
	_pms_mode = MODE_PASSIVE;
}

// Request read in Passive Mode.
void pms_requestRead()
{
	if (_pms_mode == MODE_PASSIVE)
	{
		uint8_t command[] = { 0x42, 0x4D, 0xE2, 0x00, 0x00, 0x01, 0x71 };
		SendUartMessage(_pms_uartFd, command, sizeof(command));
		//_stream->write(command, sizeof(command));
	}
}

// Non-blocking function for parse response.
bool pms_read(DATA *data)
{
	//_pms_data = data;
	//pms_loop();

	return _pms_status == STATUS_OK;
}

// Blocking function for parse response. Default timeout is 1s.
bool pms_readUntil(DATA *data, uint16_t timeout)
{
	//_pms_data = data;
	//uint32_t start = millis();
	int cnt = 0;
	do
	{
		pms_loop();
		if (_pms_status == STATUS_OK) break;
	} while (cnt++ < 1000 /*millis() - start < timeout*/);

	return _pms_status == STATUS_OK;
}

DATA pms_getData() {
	return _pms_data;
}

void pms_loop()
{
	_pms_status = STATUS_WAITING;

	const struct timespec sleepTime = { 0, 100000000 };
	for (int i = 0; i < 10; i++) {
		if (_pms_status == STATUS_OK) {
			return;
		}
	    nanosleep(&sleepTime, NULL);
	}
	//if (_stream->available())
	//{
	//	processData(_stream->read())
	//}
}

void pms_processData(uint8_t ch) {
	{
		switch (_pms_index)
		{
		case 0:
			if (ch != 0x42)
			{
				return;
			}
			_pms_calculatedChecksum = ch;
			break;

		case 1:
			if (ch != 0x4D)
			{
				_pms_index = 0;
				return;
			}
			_pms_calculatedChecksum += ch;
			break;

		case 2:
			_pms_calculatedChecksum += ch;
			_pms_frameLen = ch << 8;
			break;

		case 3:
			_pms_frameLen = ch;
			// Unsupported sensor, different frame length, transmission error e.t.c.
			if (_pms_frameLen != 2 * 9 + 2 && _pms_frameLen != 2 * 13 + 2)
			{
				_pms_index = 0;
				return;
			}
			_pms_calculatedChecksum += ch;
			break;

		default:
			if (_pms_index == _pms_frameLen + 2)
			{
				_pms_checksum = ch << 8;
			}
			else if (_pms_index == _pms_frameLen + 2 + 1)
			{
				_pms_checksum |= ch;

				if (_pms_calculatedChecksum == _pms_checksum)
				{
					// Standard Particles, CF=1.
					//if (_pms_data != NULL) {
						_pms_data.PM_SP_UG_1_0 = makeWord(_pms_payload[0], _pms_payload[1]);
						_pms_data.PM_SP_UG_2_5 = makeWord(_pms_payload[2], _pms_payload[3]);
						_pms_data.PM_SP_UG_10_0 = makeWord(_pms_payload[4], _pms_payload[5]);

						// Atmospheric Environment.
						_pms_data.PM_AE_UG_1_0 = makeWord(_pms_payload[6], _pms_payload[7]);
						_pms_data.PM_AE_UG_2_5 = makeWord(_pms_payload[8], _pms_payload[9]);
						_pms_data.PM_AE_UG_10_0 = makeWord(_pms_payload[10], _pms_payload[11]);

						_pms_data.CNT_0_3 = makeWord(_pms_payload[12], _pms_payload[13]);
						_pms_data.CNT_0_5 = makeWord(_pms_payload[14], _pms_payload[15]);
						_pms_data.CNT_1_0 = makeWord(_pms_payload[16], _pms_payload[17]);
						_pms_data.CNT_2_5 = makeWord(_pms_payload[18], _pms_payload[19]);
						_pms_data.CNT_5_0 = makeWord(_pms_payload[20], _pms_payload[21]);
						_pms_data.CNT_10_0 = makeWord(_pms_payload[22], _pms_payload[23]);
					//}

					_pms_status = STATUS_OK;					
				}

				_pms_index = 0;
				return;
			}
			else
			{
				_pms_calculatedChecksum += ch;
				uint8_t payloadIndex = _pms_index - 4;

				// Payload is common to all sensors (first 2x6 bytes).
				if (payloadIndex < sizeof(_pms_payload))
				{
					_pms_payload[payloadIndex] = ch;
				}
			}

			break;
		}

		_pms_index++;
	}
}
