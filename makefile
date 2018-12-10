test:
	g++ -o test.exe -Wpedantic -std=c++14 -pthread *.cpp

clean:
	rm -rf *.o
	rm -f *.exe
