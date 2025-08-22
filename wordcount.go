// wordcount.go - Word frequency counter
// Build: go build -ldflags="-s -w" -o wordcount_go wordcount.go
// Usage: ./wordcount_go [filename]

package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"os"
	"runtime"
	"sort"
	"sync"
	"time"
	"unsafe"
)

const (
	initialMapSize = 16384
	bufferSize = 64 * 1024 // 64KB
	maxWordLength = 100
)

type wordCount struct {
	word  string
	count int
}

var bufferPool = sync.Pool{
	New: func() interface{} {
		b := make([]byte, 0, maxWordLength)
		return &b
	},
}

// FNV-1a hash function
func fnv1aHash(data []byte) uint32 {
	hash := uint32(2166136261)
	for _, b := range data {
		hash ^= uint32(b)
		hash *= 16777619
	}
	return hash
}

func bytesToString(b []byte) string {
	return *(*string)(unsafe.Pointer(&b))
}

func extractWord(data []byte, start int, wordBuf []byte) (word []byte, newPos int, found bool) {
	pos := start
	dataLen := len(data)
	
	for pos < dataLen && !isAlpha(data[pos]) {
		pos++
	}
	
	if pos >= dataLen {
		return nil, pos, false
	}
	
	wordStart := pos
	for pos < dataLen && isAlpha(data[pos]) {
		pos++
	}
	
	wordLen := pos - wordStart
	if wordLen == 0 || wordLen > maxWordLength {
		return nil, pos, false
	}
	
	word = wordBuf[:wordLen]
	for i := 0; i < wordLen; i++ {
		word[i] = toLower(data[wordStart+i])
	}
	
	return word, pos, true
}

func isAlpha(b byte) bool {
	return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')
}

func toLower(b byte) byte {
	if b >= 'A' && b <= 'Z' {
		return b + 32
	}
	return b
}

func processFile(filename string) (map[string]int, int64, error) {
	file, err := os.Open(filename)
	if err != nil {
		return nil, 0, err
	}
	defer file.Close()

	counts := make(map[string]int, initialMapSize)
	var totalWords int64

	reader := bufio.NewReaderSize(file, bufferSize)
	
	chunk := make([]byte, bufferSize)
	var leftover []byte
	
	for {
		n, err := reader.Read(chunk)
		if n == 0 && err == io.EOF {
			break
		}
		if err != nil && err != io.EOF {
			return nil, 0, err
		}
		
		var data []byte
		if len(leftover) > 0 {
			data = append(leftover, chunk[:n]...)
			leftover = nil
		} else {
			data = chunk[:n]
		}
		
		pos := 0
		dataLen := len(data)
		wordBuf := make([]byte, 0, maxWordLength)
		
		for pos < dataLen {
			for pos < dataLen && !isAlpha(data[pos]) {
				pos++
			}
			
			if pos >= dataLen {
				break
			}
			
			wordStart := pos
			wordBuf = wordBuf[:0]
			
			for pos < dataLen && isAlpha(data[pos]) {
				if len(wordBuf) < maxWordLength {
					wordBuf = append(wordBuf, toLower(data[pos]))
				}
				pos++
			}
			
			if pos == dataLen && err != io.EOF && isAlpha(data[dataLen-1]) {
				leftover = make([]byte, dataLen-wordStart)
				copy(leftover, data[wordStart:])
				break
			}
			
			if len(wordBuf) > 0 {
				wordStr := string(wordBuf)
				counts[wordStr]++
				totalWords++
			}
		}
		
		if err == io.EOF {
			break
		}
	}
	
	return counts, totalWords, nil
}

func sortWords(counts map[string]int) []wordCount {
	sorted := make([]wordCount, 0, len(counts))
	
	for word, count := range counts {
		sorted = append(sorted, wordCount{word, count})
	}
	
	sort.Slice(sorted, func(i, j int) bool {
		if sorted[i].count != sorted[j].count {
			return sorted[i].count > sorted[j].count
		}
		return sorted[i].word < sorted[j].word
	})
	
	return sorted
}

func formatNumber(n int64) string {
	str := fmt.Sprintf("%d", n)
	if len(str) <= 3 {
		return str
	}
	
	var result []byte
	for i, digit := range str {
		if i > 0 && (len(str)-i)%3 == 0 {
			result = append(result, ',')
		}
		result = append(result, byte(digit))
	}
	return string(result)
}

func getFileSizeMB(filename string) float64 {
	info, err := os.Stat(filename)
	if err != nil {
		return 0
	}
	return float64(info.Size()) / (1024.0 * 1024.0)
}

func writeOutputFile(filename string, sorted []wordCount, totalWords int64, uniqueWords int, executionTime float64) error {
	outputFilename := filename[:len(filename)-len(".txt")] + "_go_results.txt"
	if idx := bytes.LastIndex([]byte(filename), []byte(".")); idx != -1 {
		outputFilename = filename[:idx] + "_go_results.txt"
	}
	
	file, err := os.Create(outputFilename)
	if err != nil {
		return err
	}
	defer file.Close()
	
	writer := bufio.NewWriterSize(file, 32*1024)
	defer writer.Flush()
	
	fmt.Fprintf(writer, "Word Frequency Analysis - Go Implementation\n")
	fmt.Fprintf(writer, "Input file: %s\n", filename)
	fmt.Fprintf(writer, "Generated: %s\n", time.Now().Format("2006-01-02 15:04:05"))
	fmt.Fprintf(writer, "Execution time: %.2f ms\n\n", executionTime)
	fmt.Fprintf(writer, "Total words: %s\n", formatNumber(totalWords))
	fmt.Fprintf(writer, "Unique words: %s\n\n", formatNumber(int64(uniqueWords)))
	fmt.Fprintf(writer, "Top 100 Most Frequent Words:\n")
	fmt.Fprintf(writer, "Rank  Word            Count     Percentage\n")
	fmt.Fprintf(writer, "----  --------------- --------- ----------\n")
	
	limit := 100
	if len(sorted) < limit {
		limit = len(sorted)
	}
	
	for i := 0; i < limit; i++ {
		percentage := float64(sorted[i].count) * 100.0 / float64(totalWords)
		fmt.Fprintf(writer, "%4d  %-15s %9s %10.2f%%\n",
			i+1, sorted[i].word, formatNumber(int64(sorted[i].count)), percentage)
	}
	
	fmt.Printf("\nResults written to: %s\n", outputFilename)
	return nil
}

func main() {
	filename := "book.txt"
	if len(os.Args) > 1 {
		filename = os.Args[1]
	}
	
	if _, err := os.Stat(filename); os.IsNotExist(err) {
		fmt.Fprintf(os.Stderr, "Error: File '%s' not found\n", filename)
		fmt.Println("Usage: ./wordcount_go [filename]")
		fmt.Println("\nTo create a test file:")
		fmt.Println("curl https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt")
		os.Exit(1)
	}
	
	fmt.Printf("Processing file: %s\n", filename)
	
	runtime.GC()
	
	startTime := time.Now()
	startMem := &runtime.MemStats{}
	runtime.ReadMemStats(startMem)
	
	// Process file
	counts, totalWords, err := processFile(filename)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error processing file: %v\n", err)
		os.Exit(1)
	}
	
	sorted := sortWords(counts)
	
	duration := time.Since(startTime)
	executionTime := float64(duration.Microseconds()) / 1000.0
	
	endMem := &runtime.MemStats{}
	runtime.ReadMemStats(endMem)
	memoryUsed := float64(endMem.Alloc-startMem.Alloc) / (1024.0 * 1024.0)
	
	fileSize := getFileSizeMB(filename)
	
	fmt.Println("\n=== Top 10 Most Frequent Words ===")
	limit := 10
	if len(sorted) < limit {
		limit = len(sorted)
	}
	for i := 0; i < limit; i++ {
		fmt.Printf("%2d. %-15s %9s\n", i+1, sorted[i].word, formatNumber(int64(sorted[i].count)))
	}
	
	fmt.Println("\n=== Statistics ===")
	fmt.Printf("File size:       %.2f MB\n", fileSize)
	fmt.Printf("Total words:     %s\n", formatNumber(totalWords))
	fmt.Printf("Unique words:    %s\n", formatNumber(int64(len(counts))))
	fmt.Printf("Execution time:  %.2f ms\n", executionTime)
	fmt.Printf("Memory used:     %.2f MB\n", memoryUsed)
	fmt.Printf("Go version:      %s\n", runtime.Version())
	fmt.Printf("CPU cores:       %d\n", runtime.NumCPU())
	fmt.Printf("GOMAXPROCS:      %d\n", runtime.GOMAXPROCS(0))
	
	if err := writeOutputFile(filename, sorted, totalWords, len(counts), executionTime); err != nil {
		fmt.Fprintf(os.Stderr, "Error writing output file: %v\n", err)
	}
	
}