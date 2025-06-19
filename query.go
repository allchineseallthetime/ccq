package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"hash/fnv"
	"log"
	"os"
	"strings"
	"syscall"
	"time"
	"unsafe"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/alecthomas/mph"
)

/* constants and structs */

const (
	magic         = "CUNCHUQI"
	dbPath        = "/usr/local/share/ccq/db.bin" 
	studyFileTail = "/.local/share/ccq/zh"
	fsrsInit      = "00;0250;0500"       
)

var bilingual = map[string]bool{
	"oxford": true, 
	"wenlin": true, 
}

type hashEntry struct {
	Verification uint64
	Offset       uint32
	Count        uint32
}

type dict struct {
	chd   *mph.CHD
	table []hashEntry
	pool  []byte
}

func loadDict() (*dict, error) {
	fd, err := os.Open(dbPath)
	if err != nil {
		return nil, err
	}
	info, err := fd.Stat()
	if err != nil {
		return nil, err
	}
	data, err := syscall.Mmap(int(fd.Fd()), 0, int(info.Size()),
		syscall.PROT_READ, syscall.MAP_PRIVATE)
	if err != nil {
		return nil, err
	}
	p := data
	if string(p[:8]) != magic {
		return nil, fmt.Errorf("bad magic")
	}
	p = p[8:]

	tblSz := binary.LittleEndian.Uint32(p[4:8])
	chdLen := binary.LittleEndian.Uint32(p[8:12])
	p = p[12:]

	chdBytes := p[:chdLen]
	rest := p[chdLen:]
	entrySize := uint32(unsafe.Sizeof(hashEntry{}))
	hashTabBytes := rest[:tblSz*entrySize]
	pool := rest[tblSz*entrySize:]

	chd, err := mph.Mmap(chdBytes)
	if err != nil {
		return nil, err
	}
	table := unsafe.Slice((*hashEntry)(unsafe.Pointer(&hashTabBytes[0])), tblSz)
	return &dict{chd: chd, table: table, pool: pool}, nil
}

type Entry struct{ Dict, Def string }

/* code */

/* this assumes a db.bin file in ~/.local/share/ccq/ */
func (d *dict) lookup(key string) ([]Entry, error) {
	v := d.chd.Get([]byte(key))
	if v == nil {
		return nil, fmt.Errorf("not found")
	}
	idx := binary.LittleEndian.Uint32(v)
	ent := d.table[idx]

	h := fnv.New64a(); h.Write([]byte(key))
	if h.Sum64() != ent.Verification {
		return nil, fmt.Errorf("hash mismatch")
	}

	out := make([]Entry, 0, ent.Count)
	block := d.pool[ent.Offset:]
	for i := uint32(0); i < ent.Count; i++ {
		end := bytes.IndexByte(block, 0)
		parts := strings.SplitN(string(block[:end]), "|", 3)
		if len(parts) == 3 {
			out = append(out, Entry{Dict: parts[1], Def: parts[2]})
		}
		block = block[end+1:]
	}
	return out, nil
}

type model struct {
	entries         []Entry
	mode            []string
	tabIdx          int
	cursor, offset  int
	height, width   int
}

func (m model) filtered() []Entry {
	if m.mode[m.tabIdx] == "all" {
		return m.entries
	}
	wantBil := m.mode[m.tabIdx] == "bilingual"
	out := make([]Entry, 0, len(m.entries))
	for _, e := range m.entries {
		if bilingual[e.Dict] == wantBil {
			out = append(out, e)
		}
	}
	return out
}

func (m model) Init() tea.Cmd { return nil }

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch v := msg.(type) {
	case tea.WindowSizeMsg:
		m.height, m.width = v.Height, v.Width
	case tea.KeyMsg:
		switch k := v.String(); k {
		case "left", "h":
			m.tabIdx = (m.tabIdx - 1 + len(m.mode)) % len(m.mode)
			m.cursor, m.offset = 0, 0
		case "right", "l", "tab":
			m.tabIdx = (m.tabIdx + 1) % len(m.mode)
			m.cursor, m.offset = 0, 0
		case "up", "k":
			if m.cursor > 0 {
				m.cursor--
				if m.cursor < m.offset {
					m.offset--
				}
			}
		case "down", "j":
			if m.cursor < len(m.filtered())-1 {
				m.cursor++
				vis := m.height - 5
				if m.cursor >= m.offset+vis {
					m.offset++
				}
			}
		case "enter":
			saveSelected(m)
			return m, tea.Quit
		case "q", "ctrl+c":
			return m, tea.Quit
		}
	}
	return m, nil
}

func (m model) View() string {
	var b strings.Builder
	for i, t := range m.mode {
		if i == m.tabIdx {
			fmt.Fprintf(&b, "[%s] ", strings.ToUpper(t))
		} else { fmt.Fprintf(&b, " %s  ", t) }
	}
	b.WriteString("\n\n")

	list := m.filtered()
	vis := max(m.height - 5, 1)
	end := min(m.offset + vis, len(list))

	for i := m.offset; i < end; i++ {
		prefix := "  "
		if i == m.cursor { prefix = "  ➤  " }
		e := list[i]
		fmt.Fprintf(&b, "%s[%d] [%s] %s\n", prefix, i-m.offset+1, e.Dict, e.Def)
	}
	b.WriteString("\n navigation: h,j,k,l | selection: enter | quit:  q\n")
	return b.String()
}

func saveSelected(m model) {
	list := m.filtered()
	if len(list) == 0 { return }
	e := list[m.cursor]

	home := os.Getenv("HOME")
	if home == "" { return }
	slPath := home + studyFileTail

	line := fmt.Sprintf("%d|%s|%s|%s\n", time.Now().Unix(), fsrsInit, os.Args[1], e.Def)
	f, err := os.OpenFile(slPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
	if err != nil { return }
	defer f.Close()
	_, _ = f.WriteString(line)
}

func main() {
	log.SetFlags(0)
	log.SetOutput(os.Stderr)

	if len(os.Args) != 2 {
		fmt.Fprintf(os.Stderr, "usage: %s <key>\n", os.Args[0])
		os.Exit(1)
	}
	key := os.Args[1]

	d, err := loadDict()
	if err != nil {
		log.Fatal(err)
	}
	entries, err := d.lookup(key)
	if err != nil {
		log.Fatal(err)
	}

	m := model{
		mode:    []string{"all", "bilingual", "monolingual"},
		entries: entries,
	}

	p := tea.NewProgram(m, tea.WithAltScreen())

	// Run the program and check for errors.
	// We use the blank identifier _ to discard the final model,
	// since we don't need to inspect its state after the program exits.
	if _, err := p.Run(); err != nil {
		log.Fatal(err)
	}
}
