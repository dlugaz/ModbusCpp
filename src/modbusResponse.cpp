// Modbus for c++ <https://github.com/Mazurel/Modbus>
// Copyright (c) 2020 Mateusz Mazur aka Mazurel
// Licensed under: MIT License <http://opensource.org/licenses/MIT>

#include "modbusResponse.hpp"
#include "modbusException.hpp"
#include "modbusUtils.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>

using namespace MB;

ModbusResponse::ModbusResponse(uint8_t slaveId, utils::MBFunctionCode functionCode,
                               uint16_t address, uint16_t registersNumber,
                               std::vector<ModbusCell> values)
    : _slaveID(slaveId), _functionCode(functionCode), _address(address),
      _registersNumber(registersNumber), _values(std::move(values)) {
    // Force proper modbuscell type
    switch (functionRegisters()) {
    case utils::OutputCoils:
    case utils::InputContacts:
        std::for_each(_values.begin(), _values.end(),
                      [](ModbusCell &cell) -> void { cell.coil(); });
        break;
    case utils::HoldingRegisters:
    case utils::InputRegisters:
        std::for_each(_values.begin(), _values.end(),
                      [](ModbusCell &cell) -> void { cell.reg(); });
        break;
    }
}

ModbusResponse::ModbusResponse(const ModbusResponse &reference)
    : _slaveID(reference.slaveID()), _functionCode(reference.functionCode()),
      _address(reference.registerAddress()),
      _registersNumber(reference.numberOfRegisters()),
      _values(reference.registerValues()) {}

ModbusResponse &ModbusResponse::operator=(const ModbusResponse &reference) {
    this->_slaveID         = reference.slaveID();
    this->_functionCode    = reference.functionCode();
    this->_address         = reference.registerAddress();
    this->_registersNumber = reference.numberOfRegisters();
    this->_values          = reference.registerValues();
    return *this;
}

ModbusResponse::ModbusResponse(std::vector<uint8_t> inputData, bool CRC) {
    try {
        if (inputData.size() < 3)
            throw ModbusException(utils::InputDataLengthInvalid);

        _slaveID      = inputData.at(0);
        _functionCode = static_cast<utils::MBFunctionCode>(0b01111111 & inputData.at(1));
        _exception = (inputData.at(1) & 0b10000000) != 0;

        if (functionType() != utils::Read)
            _address = utils::bigEndianConv(&inputData.at(2));

        int crcIndex = -1;

        if(!_exception){
            switch (_functionCode) {
            case utils::ReadDiscreteOutputCoils:
            case utils::ReadDiscreteInputContacts:
                _bytes            = inputData.at(2);
                _registersNumber = _bytes * 8;
                _values          = std::vector<ModbusCell>(_registersNumber);
                for (auto i = 0; i < _registersNumber; i++) {
                    _values[i].coil() = inputData.at(3 + (i / 8)) & (1 << (i % 8));
                }
                crcIndex = 2 + _bytes + 1;
                break;
            case utils::ReadAnalogOutputHoldingRegisters:
            case utils::ReadAnalogInputRegisters:
                _bytes            = inputData.at(2);
                _registersNumber = _bytes / 2;
                for (auto i = 0; i < _bytes / 2; i++) {
                    _values.emplace_back(utils::bigEndianConv(&inputData.at(3 + (i * 2))));
                }
                crcIndex = 2 + _bytes + 1;
                break;
            case utils::WriteSingleDiscreteOutputCoil:
                _bytes           = 8;
                _registersNumber = 1;
                _address         = utils::bigEndianConv(&inputData.at(2));
                _values          = {ModbusCell::initCoil(inputData.at(4) == 0xFF)};
                crcIndex         = 6;
                break;
            case utils::WriteSingleAnalogOutputRegister:
                _bytes           = 8;
                _registersNumber = 1;
                _address         = utils::bigEndianConv(&inputData.at(2));
                _values          = {ModbusCell::initReg(utils::bigEndianConv(&inputData.at(4)))};
                crcIndex         = 6;
                break;
            case utils::WriteMultipleDiscreteOutputCoils:
            case utils::WriteMultipleAnalogOutputHoldingRegisters:
                _bytes           = 8;
                _address         = utils::bigEndianConv(&inputData.at(2));
                _registersNumber = utils::bigEndianConv(&inputData.at(4));
                crcIndex         = 6;
                break;
            default:
                throw ModbusException(utils::InvalidByteOrder);
            }
        }else{
            crcIndex = 3;
        }

        _values.resize(_registersNumber);

        if (CRC) {
            if (crcIndex == -1 || static_cast<size_t>(crcIndex) + 2 > inputData.size())
                throw ModbusException(utils::InputDataLengthInvalid);

            const auto receivedCRC =
                *reinterpret_cast<const uint16_t *>(&inputData.at(crcIndex));
            const auto inputDataLen  = static_cast<std::size_t>(crcIndex);
            const auto calculatedCRC = MB::CRC::calculateCRC(inputData, inputDataLen);

            if (receivedCRC != calculatedCRC) {
                throw ModbusException(utils::InvalidCRC, _slaveID);
            }
        }
        if(_exception){
            throw ModbusException{inputData,CRC};
        }

    } catch (const ModbusException &ex) {
        throw ex;
    } catch (const std::out_of_range& inputDataInSufficient){
        throw ModbusException(utils::InputDataLengthInvalid);
    } catch (const std::exception &) {
        // TODO: Save the exception somewhere
        throw ModbusException(utils::InvalidByteOrder);
    }
}

std::string ModbusResponse::toString() const {
    std::stringstream result;

    result << utils::mbFunctionToStr(_functionCode)
           << ", from slave " + std::to_string(_slaveID);

    if (functionType() != utils::WriteSingle) {
        result << ", starting from address " + std::to_string(_address)
               << ", on " + std::to_string(_registersNumber) + " registers";
        if (functionType() == utils::WriteMultiple) {
            result << "\n values = { ";
            for (decltype(_values)::size_type i = 0; i < _values.size(); i++) {
                result << _values[i].toString() + " , ";
                if (i >= 3) {
                    result << " , ... ";
                    break;
                }
            }
            result << "}";
        }
    } else {
        result << ", starting from address " + std::to_string(_address)
               << "\nvalue = " + (*_values.begin()).toString();
    }

    return result.str();
}

std::vector<uint8_t> ModbusResponse::toRaw() const {
    // Fix for: https://github.com/Mazurel/Modbus/issues/3
    const auto longBytesToFollow = this->numberOfBytesToFollow();
    if (longBytesToFollow > 0xFF) {
        throw ModbusException(utils::NumberOfRegistersInvalid);
    }
    const uint8_t bytesToFollow = static_cast<uint8_t>(longBytesToFollow);

    std::vector<uint8_t> result;
    result.reserve(6);

    result.push_back(_slaveID);
    result.push_back(_functionCode);

    if (functionType() == utils::Read) {
        if (_values[0].isCoil()) {
            result.push_back(bytesToFollow); // number of bytes to follow
            auto end = result.size() - 1;
            for (std::size_t i = 0; i < _values.size(); i++) {
                if (i % 8 == 0) {
                    result.push_back(0x00);
                    end++;
                }
                result[end] |= _values[i].coil() << (i % 8);
            }
        } else {
            result.push_back(bytesToFollow); // number of bytes to follow
            for (auto _value : _values) {
                utils::pushUint16(result, _value.reg());
            }
        }
    } else {
        utils::pushUint16(result, _address);

        if (functionType() == utils::WriteSingle) {
            if (_values[0].isCoil()) {
                result.push_back(_values[0].coil() ? 0xFF : 0x00);
                result.push_back(0x00);
            } else {
                utils::pushUint16(result, _values[0].reg());
            }
        } else {
            utils::pushUint16(result, bytesToFollow);
        }
    }

    return result;
}
