# 1 Basic information
## 1.1 What is this project about?
This project is a tool to crack some cyphertext, encrypted by Vigenere cypher. It contains the necessary source code to build executable file and test suite. 
Based on https://www.cipherchallenge.org/wp-content/uploads/2020/12/Five-ways-to-crack-a-Vigenere-cipher.pdf article.
## 1.2 Vigenere cypher
The Vigenère cipher is a periodic polyalphabetic substitution cipher. The key is a string of characters.
To explain how the cipher works, let's first replace the characters of the key and the characters of the
plaintext by integers, where A=0, B=1, ..., Z=25. The length of the key let's call the period or L. So the
key is just a set of numbers k[0], k[1], ..., k[L-1]. Next take the plaintext and express it also as a list of
numbers: p[0], p[1], p[2], ... The text is encrypted by adding a number from the key modulo 26 to a number
from the plaintext, where we run through the key over and over again as needed as we run through the
plaintext. As an equation, the [i]th character is encrypted like this:
c[i] = (p[i] + k[i mod L]) mod 26
## 1.3 Assumptions
For this project, we assume, that maximum length of encryption key is 12. Also, we assume that plaintext is a string of characters in a lower case with no spaces, quotes, numbers or special characters. And the last - plaintext is some text, written in English. 
# 2 Necessary theory
## 2.1 Frequency tables 
The present method of breaking Vigenere cypher is called **variational method** and it requires information about letter frequencies in natural languages (English in our case) i.e. how often such a letter can be found in some text. To be more precise - we need information about tetragrams (combination of four-letter sequences) frequencies. My source for that is http://practicalcryptography.com/cryptanalysis/letter-frequencies-various-languages/english-letter-frequencies/ and file english_quadgrams.txt. This method is based on the fact that one tetragram in English is more common than another. We'll discuss later how it can be used.
## 2.2 Fitness
Fitness is a way to quantify how closely a piece of text resembles English text. One way to do this is to
compare the frequencies of tetragrams in the text with the frequency table that we built in the last
section. It turns out that throwing in a logarithm helps, too. The basic idea is to start with zero and add
the log of the value from our table for each tetragram that we find in the text that we are evaluating,
then divide by the number of tetragrams to get an average. The average is more useful than the total
because it allows our programs to make decisions independent of the length of the text. Defined in this
way, the fitness of English texts is typically around -9.6.
### pseudocode:
```
def fitness(text):
  result = 0
  for i in range(len(text)-3):
    tetragram = text[i:i+4]
    x = (ALPHABET.index(tetragram[0])*26*26*26 +
          ALPHABET.index(tetragram[1])*26*26 +
          ALPHABET.index(tetragram[2])*26 +
          ALPHABET.index(tetragram[3]))
    y = tetrafrequencies[x]
    if y == 0:
      result += -15 # some large negative number
    else:
      result += log(y)
  result = result / (len(text) - 3)
  return result
```
## 2.3 Finding the period (keylength): index of coincidence
The index of coincidence (IoC) measures the likelihood that any two characters of a text are the same.
A concise formula for the IoC is
![My Image](images/photo_5384344029774334138_m.jpg)

where n[i] are the counts of the letters in the text, and N is the total number of characters. Notice that we
used a normalization factor of 26 which does not appear in Friedman's original definition. With this
normalization, a random text has an IoC close to 1, while English text is close to 1.7.
To use the IoC to find the period of the cipher, we cut the ciphertext into m slices, where each slice
contains every mth letter. Then we find the IoC for each slice and average them. We do this for various
choices of m. The smallest m with an average IoC close to 1.7 is our period. For example, here we see
the IoC versus the number of slices m. The period for this example is 7.
![My Image](images/photo_5384344029774334139_x.jpg)

Now, we can find the length of key.
### Pseudocode:
```
def index_of_coincidence(text):
  counts = [0]*26
  for char in text:
    counts[ALPHABET.index(char)] += 1
  numer = 0
  total = 0
  for i in range(26):
    numer += counts[i]*(counts[i]-1)
    total += counts[i]
  return 26*numer / (total*(total-1))

found = False
period = 0
while not found:
  period += 1
  slices = ['']*period
  for i in range(len(ciphertext)):
    slices[i%period] += ciphertext[i]
  sum = 0
  for i in range(period):
    sum += index_of_coincidence(slices[i])
  ioc = sum / period
  if ioc > 1.6:
    found = True
```
## 2.4 Evaluation whether the text consists of English words
In case when the cyphertext is very short (13 - 30 letters) IoC method is not enough to evaluate if the text consists of English words. So, 
let's discuss the dictionary metod to evaluate it. 

This method needs the dictionary of English words. The source is http://www.mieliestronk.com/corncob_lowercase.txt

The method is simple: we iterate over whole dictionary and looking for every entry of current word in the text. We mark every letter in each such substring as a part of some word, even for overlapping cases. In the end, we divide the count of "marked" letters to total letters count. If the rate is more than 0.95 - we assume that the text is consists of English words.
### Ideas to improve it
Despite this method makes less false-positives and false-negatives than IoC method for small texts, it still makes mistakes comparely often. 
Maybe, there is a way to use "knapsack problem" solution to make such evaluation more precise
