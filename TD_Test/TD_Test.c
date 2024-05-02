#include <stdio.h>
#include "ExternalInterface.h"

int main() {
    void* tagger = getTagger();
    printf("Got Time Tagger %p", tagger);
    return 0;
}