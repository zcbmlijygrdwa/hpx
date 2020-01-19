////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2020 Agustin Berge
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
////////////////////////////////////////////////////////////////////////////////

struct empty
{
};

struct X
{
    [[no_unique_address]] int i;
    [[no_unique_address]] empty e;
};

struct Y
{
    int i;
};

int main()
{
    static_assert(sizeof(X) == sizeof(Y), "");
    return 0;
}
