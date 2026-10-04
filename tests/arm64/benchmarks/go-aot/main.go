package main

import (
	"fmt"
	"sort"
)

func main() {
	values := []int{9, 1, 7, 2, 8, 3, 6, 4, 5}
	sort.Ints(values)
	sum := 0
	for i, value := range values {
		if value != i+1 {
			panic("sort regression")
		}
		sum += value
	}
	if sum != 45 {
		panic("sum regression")
	}
	fmt.Println("GO_AOT_RUN_OK", sum)
}
