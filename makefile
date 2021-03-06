test-opt:
	g++ -o test.exe -Wall -std=c++17 -pthread *.cpp -O4

test:
	g++ -o test.exe -Wall -std=c++17 -pthread *.cpp -O0

sanitize-opt:
	clang++ -o test.exe -Wall -std=c++17 -pthread *.cpp -O3 -fsanitize=undefined -fsanitize=address

sanitize:
	clang++ -o test.exe -Wall -std=c++17 -pthread *.cpp -O0 -fsanitize=undefined -fsanitize=address

clean:
	rm -rf *.o
	rm -f *.exe
