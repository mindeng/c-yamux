package main

import (
	"bufio"
	"fmt"
	"net"

	"github.com/hashicorp/yamux"
)

func handleStream(conn net.Conn) {
	r := bufio.NewReader(conn)
	w := bufio.NewWriter(conn)
	for {
		line, _, err := r.ReadLine()

		if err != nil {
			print(err)
			panic(err)
		}

		fmt.Println(string(line))

		w.Write(line)
	}
}

func handleRequest(conn net.Conn) {
	session, err := yamux.Server(conn, nil)

	if err != nil {
		print(err)
		panic(err)
	}

	for {
		stream, err := session.Accept()

		if err != nil {
			print(err)
			panic(err)
		}

		go handleStream(stream)
	}
}

func serv() {
	l, err := net.Listen("tcp", "127.0.0.1:1337")

	if err != nil {
		print(err)
		panic(err)
	}

	for {
		conn, err := l.Accept()

		if err != nil {
			print(err)
			panic(err)
            continue
		}

		go handleRequest(conn)
	}
}
func client() {
	c, err := net.Dial("tcp", "127.0.0.1:1337")

	if err != nil {
		print(err)
		panic(err)
	}

	session, err := yamux.Client(c, nil)
	if err != nil {
		print(err)
		panic(err)
	}

	stream, err := session.Open()
	if err != nil {
		print(err)
		panic(err)
	}

	stream.Write([]byte("hello\n"))
}

func main() {
	//serv()
	client()
}

