import sys
try:
    import pygame
    from pygame.locals import *
except ImportError:
    print "=========================="
    print "You have to install pygame"
    print "=========================="
    raise

try:
    from OSC import OSCClient, OSCMessage # provided by pyOSC
except ImportError:
    print "========================="
    print "You have to install pyOSC"
    print "========================="
    raise

PORT = 4444

if len(sys.argv) != 5:
    print "usage: remote <addr> <path> <width> <height>"
    sys.exit(1)

addr, path, width, height = sys.argv[1:5]
width, height = int(width), int(height)

client = OSCClient()
client.connect((addr, PORT))

pygame.init()

screen = pygame.display.set_mode((width, height))
pygame.display.set_caption('Info Beamer Remote Control')

font = pygame.font.Font(None, 16)
text = font.render('Sending to info-beamer @ %s:%d' % (addr, PORT), True, (255, 255, 255))

screen.fill((255, 0, 0))
screen.blit(text, (
    (width - text.get_width()) / 2,
    (height - text.get_height()) / 2
))
pygame.display.flip()

while 1:
    event = pygame.event.wait()

    if event.type == KEYUP:
        msg = OSCMessage(path + "keyup")
        msg.append(event.key)
        client.send(msg)
    elif event.type == KEYDOWN:
        if event.key == K_ESCAPE:
            break
        msg = OSCMessage(path + "keydown")
        msg.append(event.key)
        client.send(msg)
    elif event.type == MOUSEBUTTONDOWN:
        msg = OSCMessage(path + "mousedown")
        msg.append(event.button)
        msg.append(event.pos[0])
        msg.append(event.pos[1])
        client.send(msg)
    elif event.type == MOUSEBUTTONUP:
        msg = OSCMessage(path + "mouseup")
        msg.append(event.button)
        msg.append(event.pos[0])
        msg.append(event.pos[1])
        client.send(msg)
    elif event.type == MOUSEMOTION:
        msg = OSCMessage(path + "mousemotion")
        msg.append(event.pos[0])
        msg.append(event.pos[1])
        client.send(msg)
