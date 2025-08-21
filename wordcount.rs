// wordcount.rs
/**
 * Word Frequency Counter - Rust Implementation
 * 
 * Optimized for performance while maintaining readability.
 * Best practices applied:
 * - Using HashMap with pre-allocated capacity
 * - Efficient string processing without regex overhead
 * - Zero-copy where possible
 * - Proper error handling with Result
 * 
 * Build: rustc -O wordcount.rs
 * Usage: ./wordcount [filename]
 * If no filename provided, defaults to 'book.txt'
 */

use std::collections::HashMap;
use std::env;
use std::fs::{self, File};
use std::io::{self, Write};
use std::time::Instant;

fn main() -> io::Result<()> {
    // Get filename from command line or use default
    let args: Vec<String> = env::args().collect();
    let filename = args.get(1).map(|s| s.as_str()).unwrap_or("book.txt");
    
    println!("Processing file: {}", filename);
    
    // Start timing
    let start_time = Instant::now();
    
    // Read entire file
    let text = match fs::read_to_string(filename) {
        Ok(content) => content,
        Err(e) => {
            eprintln!("Error: File '{}' not found", filename);
            eprintln!("Error details: {}", e);
            eprintln!("\nUsage: ./wordcount [filename]");
            eprintln!("\nTo create a test file:");
            eprintln!("curl https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt");
            std::process::exit(1);
        }
    };
    
    // Get file size for statistics
    let file_size = fs::metadata(filename)?.len() as f64 / 1024.0 / 1024.0;
    
    // Convert to lowercase for case-insensitive counting
    let text_lower = text.to_lowercase();
    
    // Pre-allocate HashMap with estimated capacity (reduces rehashing)
    // Estimate ~10,000 unique words for a typical book
    let mut counts: HashMap<String, u32> = HashMap::with_capacity(10_000);
    
    // Count word frequencies
    // Using split_whitespace() and then filtering is faster than regex
    let mut total_words = 0u64;
    
    for word in text_lower.split_whitespace() {
        // Extract only alphabetic characters from each word
        let clean_word: String = word
            .chars()
            .filter(|c| c.is_alphabetic())
            .collect();
        
        // Skip empty words
        if !clean_word.is_empty() {
            *counts.entry(clean_word).or_insert(0) += 1;
            total_words += 1;
        }
    }
    
    // Sort by frequency (descending)
    let mut sorted: Vec<(&String, &u32)> = counts.iter().collect();
    sorted.sort_unstable_by(|a, b| b.1.cmp(a.1));
    
    // Calculate execution time
    let duration = start_time.elapsed();
    let execution_time = duration.as_secs_f64() * 1000.0;
    
    // Output results
    println!("\n=== Top 10 Most Frequent Words ===");
    for (index, (word, count)) in sorted.iter().take(10).enumerate() {
        println!("{:2}. {:<15} {:>8}", index + 1, word, format_number(*count));
    }
    
    println!("\n=== Statistics ===");
    println!("File size:       {:.2} MB", file_size);
    println!("Total words:     {}", format_number(total_words as u32));
    println!("Unique words:    {}", format_number(counts.len() as u32));
    println!("Execution time:  {:.2} ms", execution_time);
    println!("Rust version:    {}", env!("RUSTC_VERSION"));
    
    println!("\nCompiled with: rustc -O (optimizations enabled)");
    
    // Write results to output file
    match write_output_file(filename, &sorted, total_words, counts.len(), execution_time) {
        Ok(_) => {},
        Err(e) => eprintln!("Failed to write output file: {}", e),
    };
    
    Ok(())
}

/// Format number with thousand separators
fn format_number(n: u32) -> String {
    let s = n.to_string();
    let mut result = String::new();
    let mut count = 0;
    
    for ch in s.chars().rev() {
        if count == 3 {
            result.push(',');
            count = 0;
        }
        result.push(ch);
        count += 1;
    }
    
    result.chars().rev().collect()
}

/// Write results to output file
fn write_output_file(
    input_filename: &str,
    sorted: &[(&String, &u32)],
    total_words: u64,
    unique_words: usize,
    execution_time: f64,
) -> io::Result<()> {
    let output_filename = input_filename
        .rsplit_once('.')
        .map(|(base, _)| format!("{}_rust_results.txt", base))
        .unwrap_or_else(|| format!("{}_rust_results.txt", input_filename));
    
    let mut file = File::create(&output_filename)?;
    
    writeln!(file, "Word Frequency Analysis - Rust Implementation")?;
    writeln!(file, "Input file: {}", input_filename)?;
    writeln!(file, "Execution time: {:.2} ms\n", execution_time)?;
    writeln!(file, "Total words: {}", format_number(total_words as u32))?;
    writeln!(file, "Unique words: {}\n", format_number(unique_words as u32))?;
    writeln!(file, "Top 100 Most Frequent Words:")?;
    writeln!(file, "Rank  Word            Count     Percentage")?;
    writeln!(file, "----  --------------- --------- ----------")?;
    
    for (index, (word, count)) in sorted.iter().take(100).enumerate() {
        let percentage = (**count as f64 * 100.0) / total_words as f64;
        writeln!(
            file,
            "{:4}  {:<15} {:>9} {:>10.2}%",
            index + 1,
            word,
            format_number(**count),
            percentage
        )?;
    }
    
    println!("\nResults written to: {}", output_filename);
    Ok(())
}

/**
 * Performance notes:
 * - HashMap::with_capacity() reduces rehashing overhead
 * - split_whitespace() is faster than regex for simple tokenization
 * - sort_unstable_by() is faster when stability isn't needed
 * - Building with -O flag enables all optimizations
 * 
 * For even better performance:
 * - Use rustc -C opt-level=3 -C target-cpu=native wordcount.rs
 * - Consider using FxHashMap or AHashMap (faster hash functions)
 * - For huge files, use BufReader with buffered reading
 * - Use rayon for parallel processing on multi-core systems
 * 
 * Advanced optimizations (requires dependencies):
 * - cargo build --release (if using Cargo)
 * - Use memmap2 for memory-mapped files
 * - Use ahash for faster hashing
 * - Use rayon for parallelization
 */

// To show Rust version at compile time, build with:
// RUSTC_VERSION=$(rustc --version | cut -d' ' -f2) rustc -O wordcount.rs