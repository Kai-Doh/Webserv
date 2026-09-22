# Builds the subject's required `webserv` binary (p.6: "Program Name:
# webserv"). srcs/harness_main.cpp's own comments explain it started as a
# throwaway proof harness for the HTTP+CGI half (RequestParser/
# RequestHandler/CgiHandler) -- it now also serves as this project's actual
# main()/event loop unless/until replaced by a teammate's Core Server.
NAME        = webserv

CXX         = c++
CXXFLAGS    = -Wall -Wextra -Werror -std=c++98 -Iinclude -Isrcs -MMD -MP

SRCS_DIR    = srcs
OBJS_DIR    = objs

# Recursive: srcs/ is organized into subfolders by module (http/, cgi/,
# config/, utils/, core/, net/, ...), so a flat wildcard would silently
# drop files.
SRCS        = $(shell find $(SRCS_DIR) -name '*.cpp')
OBJS        = $(patsubst $(SRCS_DIR)/%.cpp,$(OBJS_DIR)/%.o,$(SRCS))
DEPS        = $(OBJS:.o=.d)

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(NAME)

$(OBJS_DIR)/%.o: $(SRCS_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(OBJS_DIR)

fclean: clean
	rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re
