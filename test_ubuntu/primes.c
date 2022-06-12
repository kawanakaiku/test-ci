#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

void primes(long long n) {
  long long i;
  long long j;
  long long x;
  long long n2 = (n-1)/2 - 1;
  bool *numbers = malloc((n2+1) * sizeof(bool));

  printf("%d ", 2);

  for(i = 0;i <= n2;i++) {
    if (numbers[i]) { continue; }
    x = i*2+3;
    printf("%lld ", x);
    for(j = x*x; j <= n; j+=x*2) {
      numbers[(j-3)/2] = true;
    }
  }

  free(numbers);
  printf("\n");
}

int main(int argc, char **argv) {
  if(argc!=2) {
    return 1;
  }
  long long n = atoi(argv[1]);
  if(argc<=1) {
    return 1;
  }
  primes(n);
  return 0;
}
