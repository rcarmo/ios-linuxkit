package probe

type Pair struct{ Left, Right int }

func Sum(values []Pair) int {
	total := 0
	for _, value := range values {
		total += value.Left + value.Right
	}
	return total
}
