#!/bin/sh
# Creates the sample files the demos in tests/ use.
#
#   tests/setup.sh [DIR]      (default: ~/terminator-demo)
#
# Then start Terminator and `cd` into DIR (or `cd ~/terminator-demo`).
set -e
DIR=${1:-"$HOME/terminator-demo"}
mkdir -p "$DIR"
cd "$DIR"

# Task 10: completion (abc.txt / abcd.txt share a prefix; def.txt is unique)
printf 'alpha\n' > abc.txt
printf 'alpha beta\n' > abcd.txt
printf 'delta\n' > def.txt
printf 'a file whose name has spaces\n' > "notes for class.txt"
mkdir -p projects/src
printf 'int main(void) { return 0; }\n' > projects/src/main.c

# Tasks 2, 4: a small program that numbers the lines it reads
cat > myprog.c <<'C'
#include <stdio.h>

int main(void)
{
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, stdin))
        printf("%3d | %s", ++n, line);
    printf("myprog: read %d line(s)\n", n);
    return 0;
}
C

# Tasks 4, 5: unsorted input, and a large file for concurrent readers
printf 'orange\nbanana\ncherry\napple\nbanana\nmango\n' > somefile.txt
printf 'first line\nsecond line\nthird line\n' > infile.txt
seq 1 200000 > large.txt

# Task 5: files for `ls *.tmp | xargs rm`
touch a.tmp b.tmp c.tmp

# Task 2: a script for the `source` built-in
cat > greet.sh <<'SH'
# Run with: source greet.sh NAME...
echo "Hello, $1! ($# argument(s) given)"
echo "You are in $(basename $(pwd)), which holds $(ls | wc -l) entries"
ls *.txt | sort | head -3
SH

echo "Demo files created in $DIR"
