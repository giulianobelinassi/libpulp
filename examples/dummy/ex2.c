/*
 *  libpulp - User-space Livepatching Library
 *
 *  Copyright (C) 2020 SUSE Linux GmbH
 *
 *  This file is part of libpulp.
 *
 *  libpulp is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  libpulp is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with libpulp.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include "libdummy.h"

void *f1(void *arg __attribute__ ((unused))) {
    fprintf(stderr, "ex2: Thread 1: entering bar from f1()\n");
    while (sleeping_bar(1)) { sleep(rand() % 3); }
    fprintf(stderr, "ex2: Thread 1: left bar from f1()\n");
    return NULL;
}

void *f2(void *arg __attribute__ ((unused))) {
    fprintf(stderr, "ex2: Thread 2: entering bar from f2()\n");
    loop_bar(20);
    fprintf(stderr, "ex2: Thread 2: left bar from f2()\n");
    return NULL;
}

void *f3(void *arg __attribute__ ((unused))) {
    fprintf(stderr, "ex2: Thread 3: entering bar from f3()\n");
    while (sleeping_bar(10)) { sleep(rand() % 3); }
    fprintf(stderr, "ex2: Thread 3: left bar from f3()\n");
    return NULL;
}

int main() {
    pthread_t tid1, tid2, tid3;

    fprintf(stderr, "ex2: Hello from main thread :)\n");

    pthread_create(&tid1, NULL, f1, NULL);
    pthread_create(&tid2, NULL, f2, NULL);
    pthread_create(&tid3, NULL, f3, NULL);

    pthread_join(tid1, NULL);
    fprintf(stderr, "joined thread 1\n");
    pthread_join(tid2, NULL);
    fprintf(stderr, "joined thread 2\n");
    pthread_join(tid3, NULL);
    fprintf(stderr, "joined thread 3\n");
}
