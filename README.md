# Pakka
A command line utility for working with quake 1 and 2 .pak files, 

Why 'pakka', well pak files, and I have kids and Makka Pakka is their favourite
[In the Night Garden Character](http://www.inthenightgarden.co.uk/).

## Installation
    $ git clone https://github.com/ajbonner/pakka.git
    $ make

## Usage
Pakka has 5 major modes:

* Extract ./pakka -x <pakfile> 
* Create ./pakka -c <pakfile> <list of paths/files>
* Add to pak ./pakka -a <pakfile> <list of files>
* Delete from pak ./pakka -d <pakfile> <paths to remove>
* List contents ./pakka -t <pakfile>

## Contributing
1. Fork it!
2. Create your feature branch: `git checkout -b my-new-feature`
3. Commit your changes: `git commit -am 'Add some feature'`
4. Push to the branch: `git push origin my-new-feature`
5. Submit a pull request :D

## History
Created as an excuse to re-learn some C circa December '15-January '16

## Credits
John Carmack for not only creating quake, but then open sourcing and its
successor games. He is the reason I am a programmer today.

I am unsure who the correct author is but I followed this documentation on 
the [Quake PAK File Format](http://debian.fmi.uni-sofia.bg/~sergei/cgsr/docs/pak.txt)
in building this utility.

## License
MIT
