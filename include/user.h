#ifndef USER_H
#define USER_H

#include "protocol.h"

struct UserState {
    char uid[UID_LEN + 1] = {};
    char password[PASSWORD_LEN + 1] = {};
    int logged_in = 0;
};

#endif
