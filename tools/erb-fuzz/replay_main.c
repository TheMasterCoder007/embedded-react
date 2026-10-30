/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Runs the fuzz target over the files it is given, for a compiler with no libFuzzer (Apple's clang). It takes
 * file arguments the way a libFuzzer build does, so a crash CI found replays the same way here.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int main(int argc, char** argv)
{
    for (int index = 1; index < argc; index++)
    {
        FILE* file = fopen(argv[index], "rb");
        if (!file)
        {
            fprintf(stderr, "can't read %s\n", argv[index]);
            return 2;
        }
        fseek(file, 0, SEEK_END);
        const long length = ftell(file);
        fseek(file, 0, SEEK_SET);
        uint8_t* bytes = malloc(length ? (size_t)length : 1u);
        if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length)
        {
            fprintf(stderr, "can't read %s\n", argv[index]);
            return 2;
        }
        fclose(file);
        fprintf(stderr, "%s\n", argv[index]);
        LLVMFuzzerTestOneInput(bytes, (size_t)length);
        free(bytes);
    }
    return 0;
}
