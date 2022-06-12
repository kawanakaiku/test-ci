sudo apt-get install -y --no-install-recommends gcc-i686-linux-gnu
i686-linux-gnu-gcc -s -pipe -O3 $(dirname $0)/primes.c -o primes
