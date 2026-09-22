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
TOTAL       := $(words $(SRCS))
BAR_WIDTH   := 30

GREEN       := \033[0;32m
YELLOW      := \033[0;33m
RED         := \033[0;31m
CYAN        := \033[0;36m
BOLD        := \033[1m
RESET       := \033[0m

# Fresh progress counter for this invocation only (pure side effect at parse
# time -- never a prerequisite of anything, so it can't affect the
# no-relink guarantee below). VERBOSE=1 skips the animation and falls back
# to plain echoed commands, e.g. to debug the exact compiler invocation.
$(shell mkdir -p $(OBJS_DIR))
ifndef VERBOSE
$(shell echo 0 > $(OBJS_DIR)/.count)
endif

all: $(NAME)

$(NAME): $(OBJS)
ifdef VERBOSE
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(NAME)
else
	@printf "\n$(CYAN)Linking $(NAME)...$(RESET)\n"
	@$(CXX) $(CXXFLAGS) $(OBJS) -o $(NAME)
	@printf "$(GREEN)$(BOLD)✓ $(NAME) built successfully$(RESET) ($(TOTAL) files)\n"
endif

$(OBJS_DIR)/%.o: $(SRCS_DIR)/%.cpp
	@mkdir -p $(dir $@)
ifdef VERBOSE
	$(CXX) $(CXXFLAGS) -c $< -o $@
else
	@if $(CXX) $(CXXFLAGS) -c $< -o $@ 2>$(OBJS_DIR)/.err; then \
		n=$$(( $$(cat $(OBJS_DIR)/.count 2>/dev/null || echo 0) + 1 )); \
		echo $$n > $(OBJS_DIR)/.count; \
		filled=$$(( n * $(BAR_WIDTH) / $(TOTAL) )); \
		empty=$$(( $(BAR_WIDTH) - filled )); \
		percent=$$(( n * 100 / $(TOTAL) )); \
		bar=$$(printf '%*s' "$$filled" '' | tr ' ' '#'); \
		space=$$(printf '%*s' "$$empty" '' | tr ' ' '-'); \
		printf "\r\033[K$(YELLOW)[%s%s] %3d%%$(RESET) (%2d/%2d) Compiling %s" \
			"$$bar" "$$space" "$$percent" "$$n" "$(TOTAL)" "$<"; \
	else \
		printf "\n$(RED)$(BOLD)compilation failed:$(RESET) $<\n\n"; \
		cat $(OBJS_DIR)/.err; \
		rm -f $(OBJS_DIR)/.err; \
		exit 1; \
	fi
endif

-include $(DEPS)

clean:
	@rm -rf $(OBJS_DIR)
	@printf "$(RED)Cleaned object files$(RESET)\n"

fclean: clean
	@rm -f $(NAME)
	@printf "$(RED)Removed $(NAME)$(RESET)\n"

re: fclean all

.PHONY: all clean fclean re
