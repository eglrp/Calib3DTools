#include "messager.h"

namespace libMsg {
Messager *globalMessager = 0;
ostream cout(globalMessager);

void error(const char *msg)
{
    if (globalMessager)
        globalMessager->message(msg, M_ERROR);
    throw MyException(msg);
}

ostream::ostream(Messager * &msg) : receiverMsg(msg),
    doublePrecision(6)
{
    ss.precision(doublePrecision);
}

ostream &ostream::operator<<(const char *value)
{
    ss<<value;
    return *this;
}

ostream &ostream::operator<<(double value)
{
    ss<<value;
    return *this;
}

ostream &ostream::operator<<(int value)
{
    ss<<value;
    return *this;
}

ostream &ostream::operator<<(unsigned int value)
{
    ss<<value;
    return *this;
}

ostream &ostream::operator<<(char value)
{
    ss<<value;
    return *this;
}

ostream &ostream::operator<<(ostream & (*manipFunc)(ostream &))
{
    return (*manipFunc)(*this);
}

ostream &ostream::flush()
{
    if (this->receiverMsg) {
        this->receiverMsg->message(ss.str(), M_TEXT);
        ss.str(std::string());
        ss.clear();
    }
    return *this;
}

ostream &ostream::endl()
{
    ss.put('\n');
    return this->flush();
}

ostream &ostream::setprecision(int precision)
{
    this->doublePrecision = precision > 2 ? precision : 2;
    ss.precision(doublePrecision);
}

ostream &flush(ostream &os)
{
    return os.flush();
}

ostream &endl(ostream &os)
{
    return os.endl();
}
}

MyException::MyException(const std::string &msg) : std::runtime_error(msg)
{
}
