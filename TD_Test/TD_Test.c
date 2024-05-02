#include <stdio.h>
#include "ExternalInterface.h"

int main() {
    void* tagger = getTagger();
    printf("Got Time Tagger %p", tagger);
    if (tagger != NULL) {
        freeTagger(tagger);
    }
    return 0;
}