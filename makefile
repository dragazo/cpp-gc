test:
	g++ -o test.exe -std=c++14 -pthread *.cpp

clean:
	rm -rf *.o
	rm -f *.exe
