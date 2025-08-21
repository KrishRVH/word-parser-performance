// WordCount.cs - OPTIMIZED VERSION
/**
 * Word Frequency Counter - C# (.NET) Implementation (OPTIMIZED)
 * 
 * Major optimizations:
 * - Manual word extraction instead of regex
 * - Process characters directly without ToLowerInvariant() on entire file
 * - StringBuilder pooling for word building
 * - Optimized dictionary operations
 * - Minimal allocations
 * 
 * Build: dotnet build -c Release
 * Usage: dotnet run --configuration Release [filename]
 */

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;

class WordCount
{
    static void Main(string[] args)
    {
        // Get filename from command line or use default
        string filename = args.Length > 0 ? args[0] : "book.txt";
        
        // Check if file exists
        if (!File.Exists(filename))
        {
            Console.Error.WriteLine($"Error: File '{filename}' not found");
            Console.WriteLine("Usage: WordCount.exe [filename]");
            Console.WriteLine("\nTo create a test file:");
            Console.WriteLine("curl https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt");
            Environment.Exit(1);
        }
        
        Console.WriteLine($"Processing file: {filename}");
        
        // Start timing
        var stopwatch = Stopwatch.StartNew();
        long startMemory = GC.GetTotalMemory(false);
        
        try
        {
            // Process file and count words
            var (counts, totalWords) = ProcessFileOptimized(filename);
            
            // Sort by frequency (descending) - optimize by taking only what we need
            var sorted = counts
                .OrderByDescending(kvp => kvp.Value)
                .ThenBy(kvp => kvp.Key)
                .Take(100)
                .ToList();
            
            // Calculate statistics
            stopwatch.Stop();
            long endMemory = GC.GetTotalMemory(false);
            double executionTime = stopwatch.Elapsed.TotalMilliseconds;
            double memoryUsed = (endMemory - startMemory) / 1024.0 / 1024.0;
            double fileSize = new FileInfo(filename).Length / 1024.0 / 1024.0;
            
            // Output results to console
            Console.WriteLine("\n=== Top 10 Most Frequent Words ===");
            for (int i = 0; i < Math.Min(10, sorted.Count); i++)
            {
                var kvp = sorted[i];
                Console.WriteLine($"{i + 1,2}. {kvp.Key,-15} {kvp.Value,8:N0}");
            }
            
            Console.WriteLine("\n=== Statistics ===");
            Console.WriteLine($"File size:       {fileSize:F2} MB");
            Console.WriteLine($"Total words:     {totalWords:N0}");
            Console.WriteLine($"Unique words:    {counts.Count:N0}");
            Console.WriteLine($"Execution time:  {executionTime:F2} ms");
            Console.WriteLine($"Memory used:     {memoryUsed:F2} MB");
            Console.WriteLine($".NET version:    {Environment.Version}");
            Console.WriteLine($"64-bit process:  {Environment.Is64BitProcess}");
            Console.WriteLine($"Optimized:       YES");
            
            // Write results to output file
            WriteOutputFile(filename, sorted, totalWords, counts.Count, executionTime);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"Error processing file: {ex.Message}");
            Environment.Exit(1);
        }
    }
    
    // Optimized file processing - manual tokenization
    static (Dictionary<string, int> counts, int totalWords) ProcessFileOptimized(string filename)
    {
        var counts = new Dictionary<string, int>(10000); // Pre-size for ~10k unique words
        int totalWords = 0;
        
        // Read file with buffering
        using (var reader = new StreamReader(filename, Encoding.UTF8, true, 65536)) // 64KB buffer
        {
            var wordBuilder = new StringBuilder(100);
            int c;
            
            while ((c = reader.Read()) != -1)
            {
                char ch = (char)c;
                
                if (char.IsLetter(ch))
                {
                    // Convert to lowercase inline and append
                    wordBuilder.Append(char.ToLowerInvariant(ch));
                }
                else if (wordBuilder.Length > 0)
                {
                    // End of word - add to dictionary
                    string word = wordBuilder.ToString();
                    
                    if (counts.TryGetValue(word, out int count))
                    {
                        counts[word] = count + 1;
                    }
                    else
                    {
                        counts[word] = 1;
                    }
                    
                    totalWords++;
                    wordBuilder.Clear();
                }
            }
            
            // Handle last word if file doesn't end with non-letter
            if (wordBuilder.Length > 0)
            {
                string word = wordBuilder.ToString();
                if (counts.TryGetValue(word, out int count))
                {
                    counts[word] = count + 1;
                }
                else
                {
                    counts[word] = 1;
                }
                totalWords++;
            }
        }
        
        return (counts, totalWords);
    }
    
    static void WriteOutputFile(string inputFile, List<KeyValuePair<string, int>> sorted, 
                                int totalWords, int uniqueWords, double executionTime)
    {
        string outputFile = Path.GetFileNameWithoutExtension(inputFile) + "_csharp_results.txt";
        
        using (var writer = new StreamWriter(outputFile))
        {
            writer.WriteLine("Word Frequency Analysis - C# Implementation (Optimized)");
            writer.WriteLine($"Input file: {inputFile}");
            writer.WriteLine($"Generated: {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
            writer.WriteLine($"Execution time: {executionTime:F2} ms");
            writer.WriteLine();
            writer.WriteLine($"Total words: {totalWords:N0}");
            writer.WriteLine($"Unique words: {uniqueWords:N0}");
            writer.WriteLine();
            writer.WriteLine("Top 100 Most Frequent Words:");
            writer.WriteLine("Rank  Word            Count     Percentage");
            writer.WriteLine("----  --------------- --------- ----------");
            
            for (int i = 0; i < Math.Min(100, sorted.Count); i++)
            {
                var kvp = sorted[i];
                double percentage = (kvp.Value * 100.0) / totalWords;
                writer.WriteLine($"{i + 1,4}  {kvp.Key,-15} {kvp.Value,9:N0} {percentage,10:F2}%");
            }
        }
        
        Console.WriteLine($"\nResults written to: {outputFile}");
    }
}

/**
 * Optimization changes from original:
 * 1. Manual character-by-character processing instead of regex
 * 2. ToLowerInvariant() on individual chars, not entire file
 * 3. StringBuilder with pre-allocated capacity
 * 4. StreamReader with large buffer (64KB)
 * 5. TryGetValue pattern for dictionary updates
 * 6. Pre-sized dictionary to reduce rehashing
 * 
 * Expected performance improvement: 3-5x faster
 * Should now be only 2-3x slower than C, not 12x
 */