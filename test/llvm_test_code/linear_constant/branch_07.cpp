extern bool cond;
int main() {
	int j = 10;
  int i = j + 20;
	if (cond) {
    i = j + 2;
  }
  return 0;
}