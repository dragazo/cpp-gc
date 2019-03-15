test:
	g++ -o test.exe -Wpedantic -std=c++17 -pthread *.cpp -O4

clean:
	rm -rf *.o
	rm -f *.exe
