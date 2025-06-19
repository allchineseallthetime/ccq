WIP! I'll work on this when I have time... sorry

I'd like to rewrite the reviewer in Go and unify everything. I initially wrote it all in C, but was unhappy with the TUI. Go's Bubble Tea library is way more pleasant than ncurses.

Anyway, ccq expects this folder:
```sh
mkdir -p ~/.local/share/ccq
```

Within which you should move these files:
```sh
mv zh db.bin ~/.local/share/ccq
```

the querier depends on:
    
    - db.bin, which is a bunch of dictionaries (all converted from Yomitan) in a hash map binary format. (TODO: provide the code to convert Yomitan dictionaries to this format)

and if you wish to add words to study, or to review these words, you need:

    - zh, which is my current study list (like a deck in Anki). This is plain-text. Feel free to delete everything in it (but keep the file), I include it so that you can try the program.

now you can build the project. there's no dependencies for the reviewer:
```sh
cc -o ccq review.c -lm
```

or clang, gcc, tcc, whichever C compiler you prefer, and move the binary where you like (eg `/usr/bin` or `~/.local/bin`)

the querier has some dependencies which Go fetches for you:
```sh
go mod init
go mod tidy
go build -o ccq-query query.go
```

and likewise move the binary where you like

then to query a word:
```sh
ccq-query WORD
```

and to review the study list
```sh
ccq -nor
```

n, o, r are optional flags to change the review order. by default it's random, n is newest first (order of creation), o is oldest first (the reverse)

(there's a -q flag which shouldn't work as I changed the querying method)
