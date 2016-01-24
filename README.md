# Pakka
A command line utility for working with quake 1 and 2 .pak files.

Why 'pakka', well pak files, and I have kids and Makka Pakka is their favourite
[In the Night Garden Character](http://www.inthenightgarden.co.uk/).

## Disclaimer
I am a novice C programmer. This code is the result of a learning exercise.
Given that C programs can be very brittle, and behave in unexpected ways,
I would strongly suggest you do not use this software :) If you are feeling
very brave and do choose to use this software and find a problem, please let
me know!

## Installation
    $ git clone https://github.com/ajbonner/pakka.git
    $ make

## Supported Platforms
Little endian 32/64 bit UNIXes should be able to build and run this no problem. I have
built on OSX 10.11 (El Capitan) using Clang (Apple LLVM version 7.0.2 (clang-700.1.81)) and 
Ubuntu Linux 14.04 (Trusty) using GCC (gcc (Ubuntu 4.8.4-2ubuntu1~14.04)
4.8.4).

## Usage
Pakka has 5 major modes:

* Extract ./pakka -xf <pakfile.pak>
* Create ./pakka -cf <pakfile.pak> <list of paths/files to add>
* Add to pak ./pakka -af <pakfile.pak> <list of files to add>
* Delete from pak ./pakka -df <pakfile.pak> <list of paths to remove>
* List pak contents ./pakka -tf <pakfile.pak>

## Contributing
1. Fork it!
2. Create your feature branch: `git checkout -b my-new-feature`
3. Commit your changes: `git commit -am 'Add some feature'`
4. Push to the branch: `git push origin my-new-feature`
5. Submit a pull request :D

## History
Created as an excuse to re-learn some C circa December '15-January '16

## Credits
John Carmack for not only creating quake, but then open sourcing it and its
successor games. He is the reason I am a programmer today.

I am unsure who the correct author is but I followed this documentation on
the [Quake PAK File Format](http://debian.fmi.uni-sofia.bg/~sergei/cgsr/docs/pak.txt)
in building this utility.

## License
MIT
