// WordCount.cs - Word frequency counter
// Build: dotnet build -c Release
// Usage: dotnet run --configuration Release [filename]

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
        string filename = args.Length > 0 ? args[0] : "book.txt";
        
        if (!File.Exists(filename))
        {
            Console.Error.WriteLine($"Error: File '{filename}' not found");
            Console.WriteLine("Usage: WordCount.exe [filename]");
            Console.WriteLine("\nTo create a test file:");
            Console.WriteLine("curl https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt");
            Environment.Exit(1);
        }
        
        Console.WriteLine($"Processing file: {filename}");
        
        var stopwatch = Stopwatch.StartNew();
        long startMemory = GC.GetTotalMemory(false);
        
        try
        {
            var (counts, totalWords) = ProcessFileOptimized(filename);
            
            var sorted = counts
                .OrderByDescending(kvp => kvp.Value)
                .ThenBy(kvp => kvp.Key)
                .Take(100)
                .ToList();
            
            stopwatch.Stop();
            long endMemory = GC.GetTotalMemory(false);
            double executionTime = stopwatch.Elapsed.TotalMilliseconds;
            double memoryUsed = (endMemory - startMemory) / 1024.0 / 1024.0;
            double fileSize = new FileInfo(filename).Length / 1024.0 / 1024.0;
            
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
            
            WriteOutputFile(filename, sorted, totalWords, counts.Count, executionTime);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"Error processing file: {ex.Message}");
            Environment.Exit(1);
        }
    }
    
    static (Dictionary<string, int> counts, int totalWords) ProcessFileOptimized(string filename)
    {
        var counts = new Dictionary<string, int>(20000);
        int totalWords = 0;
        
        ReadOnlySpan<byte> fileBytes = File.ReadAllBytes(filename);
        
        if (fileBytes.Length >= 3 && 
            fileBytes[0] == 0xEF && 
            fileBytes[1] == 0xBB && 
            fileBytes[2] == 0xBF)
        {
            fileBytes = fileBytes.Slice(3);
        }
        
        Span<byte> wordBuffer = stackalloc byte[100];
        int wordLength = 0;
        
        for (int i = 0; i < fileBytes.Length; i++)
        {
            byte b = fileBytes[i];
            
            if ((b >= 65 && b <= 90) || (b >= 97 && b <= 122))
            {
                if (wordLength < wordBuffer.Length)
                {
                    wordBuffer[wordLength++] = (byte)(b >= 65 && b <= 90 ? b + 32 : b);
                }
            }
            else if (wordLength > 0)
            {
                string word = Encoding.ASCII.GetString(wordBuffer.Slice(0, wordLength));
                
                if (counts.TryGetValue(word, out int count))
                {
                    counts[word] = count + 1;
                }
                else
                {
                    counts[word] = 1;
                }
                
                totalWords++;
                wordLength = 0;
            }
        }
        
        if (wordLength > 0)
        {
            string word = Encoding.ASCII.GetString(wordBuffer.Slice(0, wordLength));
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
        
        return (counts, totalWords);
    }
    
    static void WriteOutputFile(string inputFile, List<KeyValuePair<string, int>> sorted, 
                                int totalWords, int uniqueWords, double executionTime)
    {
        string outputFile = Path.GetFileNameWithoutExtension(inputFile) + "_csharp_results.txt";
        
        using (var writer = new StreamWriter(outputFile))
        {
            writer.WriteLine("Word Frequency Analysis - C# Implementation");
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